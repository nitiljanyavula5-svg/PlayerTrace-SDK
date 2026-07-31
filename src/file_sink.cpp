// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#include "playertrace/file_sink.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "internal/file_sink_internal.hpp"
#include "internal/path_registry.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace playertrace {

namespace {

using playertrace::internal::PathClaim;
using playertrace::internal::PathKind;

// --- 64-bit file offsets ----------------------------------------------------
// ftell()/fseek() use 32-bit long on 64-bit Windows, so an NDJSON file larger
// than 2 GiB would silently misreport its position — and that position is what
// the rollback point is built from.
using Offset = long long;
constexpr Offset kBadOffset = -1;

// Reject an unsupported configuration at COMPILE time rather than truncating
// silently at 2 GiB. On POSIX the build defines _FILE_OFFSET_BITS=64 (see
// CMakeLists.txt); if a consumer compiles these sources without it on a 32-bit
// target, off_t stays 32-bit and every offset below would wrap.
static_assert(sizeof(Offset) >= 8, "PlayerTrace requires 64-bit file offsets");
#if !defined(_WIN32)
static_assert(sizeof(off_t) >= 8,
              "PlayerTrace requires a 64-bit off_t: build with "
              "_FILE_OFFSET_BITS=64 (the bundled CMakeLists.txt does this). "
              "32-bit file offsets would silently corrupt NDJSON output past "
              "2 GiB.");
#endif

Offset tell_offset(std::FILE* fp) {
#if defined(_WIN32)
  return _ftelli64(fp);
#else
  const off_t pos = ftello(fp);
  return pos < 0 ? kBadOffset : static_cast<Offset>(pos);
#endif
}

bool seek_offset(std::FILE* fp, Offset offset, int whence) {
#if defined(_WIN32)
  return _fseeki64(fp, offset, whence) == 0;
#else
  return fseeko(fp, static_cast<off_t>(offset), whence) == 0;
#endif
}

/// Establishes the true end-of-file position.
///
/// Opening with "a"/"ab" does NOT set the file position until the first write:
/// ftell() returns 0 on a freshly reopened non-empty file. Using that as a
/// rollback point truncated every previously written — and already
/// acknowledged — line. That defect is pinned by a regression test.
Offset seek_to_end(std::FILE* fp) {
  if (!seek_offset(fp, 0, SEEK_END)) {
    return kBadOffset;
  }
  return tell_offset(fp);
}

bool flush_stream(std::FILE* fp) {
  return std::fflush(fp) == 0;
}

/// Asks the OS to make already-flushed bytes durable.
bool sync_to_disk(std::FILE* fp) {
#if defined(_WIN32)
  const int fd = _fileno(fp);
  if (fd < 0) {
    return false;
  }
  return _commit(fd) == 0;
#else
  const int fd = fileno(fp);
  if (fd < 0) {
    return false;
  }
#if defined(__APPLE__)
  // fsync() on macOS does not force the drive to flush its own cache;
  // F_FULLFSYNC does. Fall back when the filesystem does not support it.
  if (fcntl(fd, F_FULLFSYNC) == 0) {
    return true;
  }
#endif
  return fsync(fd) == 0;
#endif
}

bool truncate_to(std::FILE* fp, Offset length) {
#if defined(_WIN32)
  const int fd = _fileno(fp);
  if (fd < 0) {
    return false;
  }
  return _chsize_s(fd, static_cast<__int64>(length)) == 0;
#else
  const int fd = fileno(fp);
  if (fd < 0) {
    return false;
  }
  return ftruncate(fd, static_cast<off_t>(length)) == 0;
#endif
}

/// Finds the offset just past the last '\n', i.e. the end of the last COMPLETE
/// NDJSON line. Returns 0 when the file contains no newline at all. Scans
/// backwards in blocks so a multi-gigabyte file is never loaded into memory.
bool find_last_complete_line_end(std::FILE* fp, Offset size, Offset* out) {
  // Computed in Offset, not int: the operands are small here, but a block size
  // multiplied in int and then widened is exactly how a large-file constant
  // silently overflows later.
  constexpr Offset kBlock = Offset{64} * 1024;
  std::vector<char> buffer(static_cast<std::size_t>(kBlock));
  Offset pos = size;
  while (pos > 0) {
    const Offset chunk = pos < kBlock ? pos : kBlock;
    const Offset start = pos - chunk;
    if (!seek_offset(fp, start, SEEK_SET)) {
      return false;
    }
    const std::size_t want = static_cast<std::size_t>(chunk);
    const std::size_t got = std::fread(buffer.data(), 1, want, fp);
    if (got != want) {
      return false;
    }
    for (std::size_t i = got; i > 0; --i) {
      if (buffer[i - 1] == '\n') {
        *out = start + static_cast<Offset>(i);
        return true;
      }
    }
    pos = start;
  }
  *out = 0;
  return true;
}

}  // namespace

class FileSink::Impl {
 public:
  Impl(std::string path, bool durable_sync)
      : path_(std::move(path)), durable_sync_(durable_sync) {}

  ~Impl() { close_file(); }

  const std::string& path() const noexcept { return path_; }

  void set_fault_hook(FileSinkFaultHook hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    fault_hook_ = std::move(hook);
  }

  void request_cancel() noexcept { cancelled_.store(true); }

  Status write(const EventBatch& batch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_.load()) {
      return Status(ErrorCode::SinkError,
                    "FileSink: cancelled; '" + path_ + "' is shutting down");
    }
    Status ready = ensure_open();
    if (!ready.ok()) {
      return ready;  // events retained upstream; not acknowledged
    }

    // The rollback point is the REAL end of file, established by seeking; never
    // the position of a just-opened append stream.
    const Offset start = seek_to_end(file_);
    if (start == kBadOffset) {
      return fail_hard("FileSink: could not determine the end of '" + path_ +
                       "'");
    }

    for (const auto& event : batch.events) {
      if (fault("partial_write")) {
        // Simulate a torn write: emit a prefix with no newline, then fail.
        if (!event.json.empty()) {
          std::fwrite(event.json.data(), 1, event.json.size() / 2 + 1, file_);
        }
        return roll_back(start, "FileSink: injected partial write");
      }
      if (fault("write")) {
        return roll_back(start, "FileSink: injected write failure");
      }
      if (!event.json.empty() &&
          std::fwrite(event.json.data(), 1, event.json.size(), file_) !=
              event.json.size()) {
        return roll_back(start, "FileSink: write failed for '" + path_ + "'");
      }
      if (std::fputc('\n', file_) == EOF) {
        return roll_back(start, "FileSink: write failed for '" + path_ + "'");
      }
    }

    if (fault("flush")) {
      return roll_back(start, "FileSink: injected flush failure");
    }
    if (!flush_stream(file_)) {
      return roll_back(start, "FileSink: flush failed for '" + path_ + "'");
    }

    if (durable_sync_) {
      if (fault("sync")) {
        return roll_back(start, "FileSink: injected sync failure");
      }
      if (!sync_to_disk(file_)) {
        return roll_back(start,
                         "FileSink: durable sync failed for '" + path_ + "'");
      }
    }
    return Status();
  }

  Status flush_now() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!failure_.ok()) {
      return failure_;
    }
    if (file_ == nullptr) {
      return Status();
    }
    if (fault("flush")) {
      return Status(ErrorCode::SinkError, "FileSink: injected flush failure");
    }
    if (!flush_stream(file_)) {
      return Status(ErrorCode::SinkError,
                    "FileSink: flush failed for '" + path_ + "'");
    }
    if (durable_sync_) {
      if (fault("sync")) {
        return Status(ErrorCode::SinkError, "FileSink: injected sync failure");
      }
      if (!sync_to_disk(file_)) {
        return Status(ErrorCode::SinkError,
                      "FileSink: durable sync failed for '" + path_ + "'");
      }
    }
    return Status();
  }

  /// Claims the path exclusively for this process.
  Status claim_path() {
    std::string error;
    if (!claim_.acquire(path_, PathKind::Output, &error)) {
      failure_ = Status(ErrorCode::InvalidConfig, error);
    }
    return failure_;
  }

  const Status& failure() const { return failure_; }

 private:
  bool fault(const char* operation) const {
    return fault_hook_ && fault_hook_(operation);
  }

  Status ensure_open() {
    if (!failure_.ok()) {
      // A failed rollback leaves the file in an unknown state. Continuing would
      // risk appending after a torn line, so the sink stays failed for good.
      return failure_;
    }
    if (file_ != nullptr) {
      return Status();
    }
    if (fault("open")) {
      return Status(ErrorCode::SinkError, "FileSink: injected open failure");
    }
    // "r+b" so the tail can be inspected and repaired; created when absent.
    file_ = std::fopen(path_.c_str(), "r+b");
    if (file_ == nullptr) {
      file_ = std::fopen(path_.c_str(), "w+b");
    }
    if (file_ == nullptr) {
      return Status(ErrorCode::SinkError,
                    "FileSink: could not open '" + path_ + "'");
    }
    return repair_tail();
  }

  /// A previous process may have died mid-line. Appending after that fragment
  /// would produce a permanently unparseable line, so an incomplete tail is
  /// removed durably before anything new is written.
  Status repair_tail() {
    const Offset size = seek_to_end(file_);
    if (size == kBadOffset) {
      return fail_hard("FileSink: could not size '" + path_ + "'");
    }
    if (size == 0) {
      return Status();
    }
    Offset good_end = 0;
    if (!find_last_complete_line_end(file_, size, &good_end)) {
      return fail_hard("FileSink: could not inspect the tail of '" + path_ +
                       "'");
    }
    if (good_end == size) {
      return seek_offset(file_, 0, SEEK_END)
                 ? Status()
                 : fail_hard("FileSink: could not seek '" + path_ + "'");
    }
    // Same ordering rule as roll_back: empty the buffer, then truncate.
    std::fflush(file_);
    std::clearerr(file_);
    if (fault("truncate") || !truncate_to(file_, good_end) ||
        !sync_to_disk(file_) || !seek_offset(file_, 0, SEEK_END)) {
      return fail_hard(
          "FileSink: could not repair the incomplete final line of '" + path_ +
          "'");
    }
    return Status();
  }

  /// Rolls the file back to `good_offset` after a failed write. If the rollback
  /// cannot be completed durably the sink fails permanently, rather than being
  /// left able to append after a fragment.
  Status roll_back(Offset good_offset, const std::string& message) {
    if (file_ == nullptr || good_offset < 0) {
      return fail_hard(message);
    }
    // Push any buffered bytes out to the OS BEFORE truncating. Truncation acts
    // on the file descriptor, so bytes still sitting in the stdio buffer would
    // otherwise be written again by the next flush and reappear past the point
    // we just cut back to. The stream may already be in an error state, so its
    // result is not decisive here — the truncation below is what matters.
    std::clearerr(file_);
    std::fflush(file_);
    std::clearerr(file_);

    if (fault("truncate") || !truncate_to(file_, good_offset)) {
      return fail_hard(message + "; rollback failed");
    }
    // The buffer is already empty, so only the OS-level sync is needed.
    if (!sync_to_disk(file_)) {
      return fail_hard(message + "; rollback could not be made durable");
    }
    std::clearerr(file_);
    if (!seek_offset(file_, 0, SEEK_END)) {
      return fail_hard(message + "; could not reposition after rollback");
    }
    return Status(ErrorCode::SinkError, message);
  }

  /// Puts the sink into a permanent failed state.
  Status fail_hard(const std::string& message) {
    close_file();
    failure_ = Status(ErrorCode::SinkError,
                      message +
                          " (this FileSink is now permanently failed; events "
                          "remain in durable storage)");
    return failure_;
  }

  void close_file() {
    if (file_ != nullptr) {
      std::fflush(file_);
      std::fclose(file_);
      file_ = nullptr;
    }
  }

  std::string path_;
  bool durable_sync_;
  std::FILE* file_ = nullptr;
  std::mutex mutex_;
  FileSinkFaultHook fault_hook_;
  Status failure_;
  std::atomic<bool> cancelled_{false};
  PathClaim claim_;
};

FileSink::FileSink(std::string path, bool durable_sync)
    : impl_(new Impl(std::move(path), durable_sync)) {
  // Claim here rather than at first write so a duplicate-path mistake is
  // visible immediately through failed(); writes also report it.
  impl_->claim_path();
}

FileSink::~FileSink() = default;

Status FileSink::write(const EventBatch& batch) {
  return impl_->write(batch);
}

Status FileSink::flush() {
  return impl_->flush_now();
}

void FileSink::request_cancel() noexcept {
  impl_->request_cancel();
}

Status FileSink::failed() const {
  return impl_->failure();
}

const std::string& FileSink::path() const noexcept {
  return impl_->path();
}

void FileSinkInternal::set_fault_hook(FileSink& sink, FileSinkFaultHook hook) {
  sink.impl_->set_fault_hook(std::move(hook));
}

}  // namespace playertrace
