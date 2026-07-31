// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef PLAYERTRACE_FILE_SINK_HPP
#define PLAYERTRACE_FILE_SINK_HPP

#include <memory>
#include <string>

#include "sink.hpp"

namespace playertrace {

/// A production sink that appends each event as one line of newline-delimited
/// JSON (NDJSON) to a file.
///
/// Durability: when `durable_sync` is enabled (the default), write() returns Ok
/// only after the bytes have been flushed AND handed to the operating system's
/// durable-sync primitive (fsync/FlushFileBuffers). Only then may the pipeline
/// acknowledge and delete the corresponding rows from durable storage. With
/// `durable_sync` disabled, write() returns Ok once the data reaches the OS
/// page cache, which is faster but can lose acknowledged events if the machine
/// loses power — see docs/reliability.md.
///
/// Failure handling: a failed open, write, flush, or sync returns SinkError and
/// truncates the file back to the end of the last COMPLETE line, so a retry can
/// never append after a half-written record. The rollback point is the real end
/// of the file, obtained by seeking — not the position of a freshly opened
/// append stream, which is zero until the first write.
///
/// If a rollback cannot itself be completed durably, the sink enters a
/// PERMANENT failed state and every later call returns that error. It never
/// continues writing into a file whose contents it can no longer reason about;
/// the events simply stay in durable storage. Query failed() to detect this.
///
/// Torn tails: when opening an existing file whose last line has no terminating
/// newline (a previous process died mid-write), that fragment is durably
/// removed before anything is appended, so every completed line in the file is
/// always valid JSON.
///
/// Exclusive ownership: a path may be used by only one live FileSink per
/// process. Constructing a second FileSink on the same path leaves it in a
/// failed state (see failed()), because two instances would interleave writes
/// and one instance's rollback could truncate the other's output.
///
/// Large files: 64-bit offsets are used throughout, so files beyond 2 GiB are
/// handled correctly on all supported platforms.
class FileSink : public Sink {
 public:
  explicit FileSink(std::string path, bool durable_sync = true);
  ~FileSink() override;

  FileSink(const FileSink&) = delete;
  FileSink& operator=(const FileSink&) = delete;

  /// Appends one NDJSON line per event and, unless durable sync is disabled,
  /// syncs the file before returning Ok.
  Status write(const EventBatch& batch) override;

  /// Flushes and (unless disabled) durably syncs any buffered output.
  Status flush() override;

  /// Makes subsequent write()/flush() calls fail fast during shutdown.
  void request_cancel() noexcept override;

  /// Non-ok once the sink has failed permanently — a duplicate path, or a
  /// rollback that could not be completed. Ok otherwise.
  Status failed() const;

  const std::string& path() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;

  // Grants the internal, uninstalled test-support struct access for
  // deterministic fault injection. Never used by application code.
  friend struct FileSinkInternal;
};

}  // namespace playertrace

#endif  // PLAYERTRACE_FILE_SINK_HPP
