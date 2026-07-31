// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef PLAYERTRACE_SINK_HPP
#define PLAYERTRACE_SINK_HPP

#include <string>
#include <vector>

#include "result.hpp"

namespace playertrace {

/// One serialized event handed to a sink. Format-neutral: `json` is a single
/// JSON object as a string WITHOUT a trailing newline. Each sink chooses how to
/// frame a batch — FileSink appends '\n' per line; a future HTTP sink can join
/// into a JSON array or wrap in a request envelope without reparsing anything.
struct SerializedEvent {
  std::string event_id;  ///< For acknowledgment and downstream deduplication.
  std::string json;      ///< One JSON object; no trailing newline.
};

/// A batch of serialized events delivered to a sink in one call.
struct EventBatch {
  std::vector<SerializedEvent> events;
};

/// The extension point for delivering events. Implementations must be
/// replaceable and mockable. The v0.2 HTTP sink will implement this interface.
///
/// Contract:
///  - Return Status::ok() ONLY if the whole batch was durably handed off. On
///    any non-ok status the events are retained in durable storage and retried.
///  - Implementations must not throw. If one does, the worker treats it as a
///    SinkError and retains the batch (one bad sink never crashes the game).
///  - write() is called only from the single background worker thread, so
///    implementations do not need to be internally synchronized for that path
///    (though FileSink guards itself so it can also be shared/inspected).
class Sink {
 public:
  virtual ~Sink() = default;

  virtual Status write(const EventBatch& batch) = 0;

  /// Optional: flush any buffered output to its final destination.
  virtual Status flush() { return Status(); }

  /// Cooperative cancellation hint, invoked when the client begins shutting
  /// down. It is called from the thread calling shutdown() — NOT the worker —
  /// and may run concurrently with write() on the worker thread.
  ///
  /// An implementation should make any in-progress and any subsequent
  /// write()/flush() return promptly. Returning a non-ok Status is the right
  /// answer: the events stay in durable storage and are retried next run.
  ///
  /// This is advisory, and it is the ONLY mechanism by which a custom sink can
  /// make shutdown bounded. PlayerTrace never abandons a running worker thread,
  /// because that thread can reach state captured by your sink and your
  /// callbacks. A sink that ignores this hint and blocks forever will therefore
  /// block shutdown's completion and, eventually, ~Client(). See
  /// docs/reliability.md.
  ///
  /// Must not throw, and must be safe to call concurrently with write().
  virtual void request_cancel() noexcept {}
};

}  // namespace playertrace

#endif  // PLAYERTRACE_SINK_HPP
