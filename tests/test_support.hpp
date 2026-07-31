// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Shared test doubles and helpers. Determinism comes from flush()/join() and
// explicit latches — never from sleeping for an arbitrary interval and hoping.
#ifndef PLAYERTRACE_TESTS_TEST_SUPPORT_HPP
#define PLAYERTRACE_TESTS_TEST_SUPPORT_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "playertrace/client.hpp"
#include "playertrace/config.hpp"
#include "playertrace/sink.hpp"

namespace pt_test {

/// A sink that records every event it receives, in order. Thread-safe.
class CollectingSink : public playertrace::Sink {
 public:
  playertrace::Status write(const playertrace::EventBatch& batch) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& e : batch.events) {
      events_.push_back(e);
    }
    ++writes_;
    return playertrace::Status();
  }

  playertrace::Status flush() override {
    flushes_.fetch_add(1);
    return playertrace::Status();
  }

  std::size_t count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.size();
  }

  std::size_t writes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return writes_;
  }

  int flushes() const { return flushes_.load(); }

  std::vector<playertrace::SerializedEvent> events() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<playertrace::SerializedEvent> events_;
  std::size_t writes_ = 0;
  std::atomic<int> flushes_{0};
};

/// A sink that always fails (simulating an unavailable destination).
class FailingSink : public playertrace::Sink {
 public:
  playertrace::Status write(const playertrace::EventBatch&) override {
    calls_.fetch_add(1);
    return playertrace::Status(playertrace::ErrorCode::SinkError,
                               "test: sink unavailable");
  }
  int calls() const { return calls_.load(); }

 private:
  std::atomic<int> calls_{0};
};

/// A sink that throws (verifies the worker contains sink exceptions).
class ThrowingSink : public playertrace::Sink {
 public:
  playertrace::Status write(const playertrace::EventBatch&) override {
    throw std::runtime_error("test: sink threw");
  }
};

/// A sink that blocks inside write() until released, recording what it
/// receives.
///
/// Besides testing a wedged sink, this is the reliable way to pin the worker
/// while a test fills the in-memory queue: parked inside write() the worker
/// cannot drain, so queue-overflow behavior becomes deterministic instead of a
/// race against the batch threshold.
class BlockingSink : public playertrace::Sink {
 public:
  playertrace::Status write(const playertrace::EventBatch& batch) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      entered_ = true;
    }
    entered_cv_.notify_all();
    std::unique_lock<std::mutex> lock(mutex_);
    released_cv_.wait(lock, [this] { return released_; });
    for (const auto& e : batch.events) {
      events_.push_back(e);
    }
    return playertrace::Status();
  }

  /// Blocks until the worker is actually inside write(). False on timeout.
  bool wait_until_entered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return entered_cv_.wait_for(lock, timeout, [this] { return entered_; });
  }

  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    released_cv_.notify_all();
  }

  std::vector<playertrace::SerializedEvent> events() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
  }

  std::size_t count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.size();
  }

  ~BlockingSink() override { release(); }

 private:
  mutable std::mutex mutex_;
  std::condition_variable entered_cv_;
  std::condition_variable released_cv_;
  std::vector<playertrace::SerializedEvent> events_;
  bool entered_ = false;
  bool released_ = false;
};

/// A sink that blocks inside write() but honors request_cancel(), which is the
/// documented way a custom sink keeps shutdown bounded.
class CooperativeBlockingSink : public playertrace::Sink {
 public:
  playertrace::Status write(const playertrace::EventBatch&) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      entered_ = true;
    }
    entered_cv_.notify_all();
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return released_ || cancelled_; });
    if (cancelled_ && !released_) {
      return playertrace::Status(playertrace::ErrorCode::SinkError,
                                 "test: cancelled during shutdown");
    }
    return playertrace::Status();
  }

  void request_cancel() noexcept override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancelled_ = true;
      saw_cancel_ = true;
    }
    cv_.notify_all();
  }

  bool wait_until_entered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return entered_cv_.wait_for(lock, timeout, [this] { return entered_; });
  }

  bool saw_cancel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return saw_cancel_;
  }

  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    cv_.notify_all();
  }

  ~CooperativeBlockingSink() override { release(); }

 private:
  mutable std::mutex mutex_;
  std::condition_variable entered_cv_;
  std::condition_variable cv_;
  bool entered_ = false;
  bool released_ = false;
  bool cancelled_ = false;
  bool saw_cancel_ = false;
};

/// Invokes a caller-supplied hook on every write, then records the batch.
/// Handy for driving races from inside the worker thread deterministically.
class HookSink : public playertrace::Sink {
 public:
  explicit HookSink(std::function<void(const playertrace::EventBatch&)> hook)
      : hook_(std::move(hook)) {}

  playertrace::Status write(const playertrace::EventBatch& batch) override {
    if (hook_) {
      hook_(batch);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& e : batch.events) {
      events_.push_back(e);
    }
    return playertrace::Status();
  }

  std::vector<playertrace::SerializedEvent> events() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
  }

 private:
  std::function<void(const playertrace::EventBatch&)> hook_;
  mutable std::mutex mutex_;
  std::vector<playertrace::SerializedEvent> events_;
};

/// A per-process token. Test binaries are launched more than once against the
/// same working directory (ctest registers several filters, and the crash
/// helper runs as a child), and a test that deliberately leaves a worker thread
/// detached can still hold its database open, so a leftover file may survive.
/// Including the process id keeps runs from ever reusing each other's files.
inline const std::string& process_token() {
  static const std::string token = [] {
#if defined(_WIN32)
    return std::to_string(static_cast<unsigned long>(_getpid()));
#else
    return std::to_string(static_cast<unsigned long>(getpid()));
#endif
  }();
  return token;
}

/// Flushes and reports whether everything accepted before the flush reached
/// DURABLE STORAGE, tolerating a sink that is deliberately failing.
///
/// flush() returns Ok only when pre-flush events are also delivered and
/// acknowledged, so a test using FailingSink/ThrowingSink correctly gets
/// SinkError. That is the persisted-but-undelivered case, which is exactly what
/// those tests want to reach.
inline bool flush_persisted(
    playertrace::Client& client,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const playertrace::Status s = client.flush(timeout);
  return s.ok() || s.code() == playertrace::ErrorCode::SinkError;
}

/// Returns a unique database path in the current working directory.
inline std::string unique_db_path() {
  static std::atomic<int> counter{0};
  return "pt_test_" + process_token() + "_" +
         std::to_string(counter.fetch_add(1)) + ".db";
}

/// Returns a unique output-file path in the current working directory.
inline std::string unique_out_path() {
  static std::atomic<int> counter{0};
  return "pt_test_out_" + process_token() + "_" +
         std::to_string(counter.fetch_add(1)) + ".ndjson";
}

/// Removes a database file and its WAL/SHM sidecars.
inline void remove_db(const std::string& path) {
  std::remove(path.c_str());
  std::remove((path + "-wal").c_str());
  std::remove((path + "-shm").c_str());
}

/// RAII cleanup for a database path.
struct ScopedDb {
  explicit ScopedDb(std::string p) : path(std::move(p)) { remove_db(path); }
  ~ScopedDb() { remove_db(path); }
  ScopedDb(const ScopedDb&) = delete;
  ScopedDb& operator=(const ScopedDb&) = delete;
  std::string path;
};

/// RAII cleanup for an output file path.
struct ScopedFile {
  explicit ScopedFile(std::string p) : path(std::move(p)) {
    std::remove(path.c_str());
  }
  ~ScopedFile() { std::remove(path.c_str()); }
  ScopedFile(const ScopedFile&) = delete;
  ScopedFile& operator=(const ScopedFile&) = delete;
  std::string path;
};

/// Reads a file as a list of non-empty lines.
inline std::vector<std::string> read_lines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      lines.push_back(line);
    }
  }
  return lines;
}

/// Reads a whole file as raw bytes.
inline std::string read_all(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

}  // namespace pt_test

#endif  // PLAYERTRACE_TESTS_TEST_SUPPORT_HPP
