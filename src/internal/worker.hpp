// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Internal header — NOT installed and NOT part of the public API.
#ifndef PLAYERTRACE_INTERNAL_WORKER_HPP
#define PLAYERTRACE_INTERNAL_WORKER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "internal/event_queue.hpp"
#include "internal/json_serializer.hpp"
#include "internal/pipeline.hpp"
#include "playertrace/result.hpp"

namespace playertrace {
namespace internal {

/// The body of the background worker.
///
/// It is reference-counted so the thread body never dereferences `Worker`
/// itself. The thread is ALWAYS joined before the owning Client is destroyed —
/// PlayerTrace does not abandon it — because the thread can reach state
/// captured by the user's sink and callbacks, which shared_ptr ownership of the
/// outer objects does not protect.
class WorkerCore {
 public:
  explicit WorkerCore(std::shared_ptr<Pipeline> pipeline)
      : pipeline_(std::move(pipeline)) {}

  /// Thread entry point. Contains every exception; on an unexpected exit it
  /// records a terminal status so waiters and later calls learn the truth
  /// instead of receiving a stale Ok.
  void run();

  void request_stop();
  bool stopped() const { return stop_.load(); }

  /// Requests a flush covering every event admitted at or below
  /// `barrier_ordinal`, and returns the token to wait on.
  std::uint64_t submit_flush(std::uint64_t barrier_ordinal);

  /// Waits until a flush pass at or beyond `token` completes. Returns false on
  /// timeout. On success *out receives that pass's status: Ok only when nothing
  /// admitted at or below the barrier remains queued, retrying,
  /// persisted-unsent, or unacknowledged.
  bool wait_flush(std::uint64_t token, std::chrono::milliseconds timeout,
                  Status* out);

  /// Blocks until the worker loop has exited, or `timeout` elapses.
  bool wait_finished(std::chrono::milliseconds timeout);
  bool finished() const { return finished_.load(); }

  /// Non-ok if the loop exited unexpectedly rather than by request.
  Status terminal_status() const;

  /// Clears accumulated persistence errors after a consent purge.
  void forget_errors();

  /// Drops events drained but not yet persisted. Called under the admission
  /// lock during revocation, before the durable purge.
  void discard_in_flight();

 private:
  void process_incoming(std::vector<QueuedEvent>& drained);
  /// Delivers everything currently persisted. Returns true if any batch was
  /// acknowledged, which the flush loop uses as its progress signal.
  bool deliver_all();
  void purge_store();
  Status safe_write(const EventBatch& batch);
  Status safe_flush_sink();
  void note_error(const Status& status);
  /// True when anything admitted at or below `barrier` is still outstanding.
  bool work_remains_at_or_below(std::uint64_t barrier, Status* why);
  std::uint64_t retry_front_ordinal();
  void take_retry(std::vector<QueuedEvent>* out);
  void hold_for_retry(std::vector<QueuedEvent>&& batch);

  std::shared_ptr<Pipeline> pipeline_;
  JsonSerializer serializer_;

  /// Batches whose durable write failed are held here rather than pushed back
  /// into the bounded queue: requeueing could exceed max_queue_size and corrupt
  /// the accounting.
  std::vector<QueuedEvent> retry_;
  std::mutex retry_mutex_;

  std::atomic<bool> stop_{false};
  std::atomic<bool> finished_{false};
  std::atomic<std::uint64_t> flush_seq_{0};
  std::atomic<std::uint64_t> flush_done_{0};
  std::atomic<std::uint64_t> flush_barrier_{0};

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  Status pending_error_;    ///< Error seen since the last flush pass.
  Status terminal_status_;  ///< Set when the loop exits unexpectedly.

  /// Outcome recorded for EACH completed flush token, keyed by that exact
  /// token. One pass can answer several tokens (they coalesce), so the pass
  /// writes one entry per token it covered.
  ///
  /// Lookup is by exact key. A nearest-match lookup was wrong: once retention
  /// pruned an old token, the search slid forward onto a NEWER token's entry
  /// and reported an unrelated flush's status as if it were this one's. A token
  /// whose entry is gone is now reported as expired instead — see
  /// `flush_result_locked`.
  std::map<std::uint64_t, Status> pass_results_;

  /// Retention is bounded purely by count, which is what stops abandoned or
  /// timed-out tokens from leaking records indefinitely. Pruning against the
  /// set of currently-blocked waiters was tried and is WRONG: a token whose
  /// pass has completed may be read later, when it has no live waiter, and its
  /// record would already have been dropped — collapsing it onto a newer pass,
  /// which is the exact conflation this replaces.
  static constexpr std::size_t kMaxRetainedPasses = 256;

  /// Requires mutex_. Drops the oldest records once retention is exceeded.
  void prune_pass_results_locked();

  /// Requires mutex_. THIS token's recorded outcome, or a truthful non-Ok
  /// status when the record has been pruned or never existed. Never returns
  /// another token's result, and never returns Ok for a token it cannot
  /// account for.
  Status flush_result_locked(std::uint64_t token) const;
};

/// Owning handle for the worker thread. The thread is never detached.
class Worker {
 public:
  explicit Worker(std::shared_ptr<Pipeline> pipeline)
      : core_(std::make_shared<WorkerCore>(std::move(pipeline))) {}

  ~Worker();

  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  void start();

  /// Signals the worker to stop. Does not wait.
  void request_stop() { core_->request_stop(); }

  /// Waits up to `timeout` for the loop to exit and joins if it did. Returns
  /// true only when the thread has genuinely finished and been joined.
  bool wait_and_join(std::chrono::milliseconds timeout);

  /// Joins unconditionally, blocking for as long as the worker takes. Used by
  /// the destructor, which must not leave a running thread behind.
  void join_blocking();

  bool finished() const { return core_->finished(); }
  Status terminal_status() const { return core_->terminal_status(); }

  std::uint64_t submit_flush(std::uint64_t barrier) {
    return core_->submit_flush(barrier);
  }
  bool wait_flush(std::uint64_t token, std::chrono::milliseconds timeout,
                  Status* out) {
    return core_->wait_flush(token, timeout, out);
  }
  void forget_errors() { core_->forget_errors(); }
  void discard_in_flight() { core_->discard_in_flight(); }

  /// True when called from the worker thread itself (inside a sink or
  /// callback).
  bool is_current_thread() const {
    return started_ && std::this_thread::get_id() == thread_id_;
  }

 private:
  std::shared_ptr<WorkerCore> core_;
  std::thread thread_;
  std::thread::id thread_id_;
  bool started_ = false;
  bool joined_ = false;
  std::mutex join_mutex_;
};

}  // namespace internal
}  // namespace playertrace

#endif  // PLAYERTRACE_INTERNAL_WORKER_HPP
