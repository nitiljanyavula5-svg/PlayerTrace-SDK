// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Internal header — NOT installed and NOT part of the public API.
#ifndef PLAYERTRACE_INTERNAL_EVENT_QUEUE_HPP
#define PLAYERTRACE_INTERNAL_EVENT_QUEUE_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "playertrace/event.hpp"

namespace playertrace {
namespace internal {

/// An event plus the admission metadata the pipeline needs. Kept internal so
/// the public Event type stays a clean description of the wire schema.
struct QueuedEvent {
  Event event;
  /// Monotonic admission ordinal. Delivery order is defined by this value, not
  /// by the wall clock, so a clock that steps backwards cannot reorder events.
  std::uint64_t ordinal = 0;
  /// The consent generation in force when the event was admitted. Work from an
  /// older generation is dropped, so re-granting consent cannot revive it.
  std::uint64_t consent_generation = 0;
};

/// A bounded, thread-safe multi-producer / single-consumer queue.
///
/// Overflow policy is RejectNewest: producers check `full()` (under the
/// pipeline's admission lock, which serializes all producers) before consuming
/// a sequence number, so a rejected event never leaves a sequence gap.
///
/// The consumer (worker) uses pop_wait with a batch threshold so it wakes only
/// when a full batch is available, a timeout elapses, or it is explicitly
/// kicked.
class EventQueue {
 public:
  EventQueue(std::size_t capacity, std::size_t batch_threshold)
      : capacity_(capacity), batch_threshold_(batch_threshold) {}

  /// True if a further push would be rejected. Callers hold the admission lock,
  /// which serializes producers; only the worker removes events, so a false
  /// answer cannot become true underneath the caller.
  bool full() const;

  /// True if `count` further pushes would ALL be accepted. Same reasoning as
  /// full(): producers are serialized and only the worker frees space, so a
  /// true answer cannot become false underneath the caller. Used to reserve
  /// room for a multi-event transition before any part of it is admitted.
  bool has_room_for(std::size_t count) const;

  /// Producer side. Returns false if the queue is full (event dropped).
  bool try_push(QueuedEvent event);

  /// Admission ordinal of the oldest queued event, or UINT64_MAX when empty.
  ///
  /// Events enter in admission order, so the front is always the minimum. A
  /// flush uses this to decide whether anything admitted at or before its
  /// barrier is still waiting. Failed batches are NOT returned here — the
  /// worker holds them in its own retry buffer, because pushing them back would
  /// exceed max_queue_size and corrupt the accounting.
  std::uint64_t front_ordinal() const;

  /// Consumer side. Blocks until at least `batch_threshold` events are queued,
  /// the queue is kicked, or `timeout` elapses; then moves up to `max` events
  /// into `out`. Returns the number moved. `out` is not cleared first.
  std::size_t pop_wait(std::size_t max, std::chrono::milliseconds timeout,
                       std::vector<QueuedEvent>* out);

  /// Wakes a blocked consumer immediately (used for flush/stop/purge nudges).
  void kick();

  /// Discards all queued events and returns how many were dropped.
  std::size_t clear();

  std::size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<QueuedEvent> queue_;
  const std::size_t capacity_;
  const std::size_t batch_threshold_;
  bool kicked_ = false;
};

}  // namespace internal
}  // namespace playertrace

#endif  // PLAYERTRACE_INTERNAL_EVENT_QUEUE_HPP
