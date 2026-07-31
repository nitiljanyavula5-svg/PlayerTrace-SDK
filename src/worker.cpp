// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#include "internal/worker.hpp"

#include <exception>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

namespace playertrace {
namespace internal {

namespace {
constexpr std::uint64_t kNoOrdinal =
    (std::numeric_limits<std::uint64_t>::max)();
}  // namespace

// ---------------------------------------------------------------------------
// WorkerCore
// ---------------------------------------------------------------------------

void WorkerCore::request_stop() {
  stop_.store(true);
  pipeline_->queue().kick();
}

std::uint64_t WorkerCore::submit_flush(std::uint64_t barrier_ordinal) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Barriers only move forward: a later flush must cover everything an
    // earlier one did.
    if (barrier_ordinal > flush_barrier_.load()) {
      flush_barrier_.store(barrier_ordinal);
    }
  }
  const std::uint64_t token = flush_seq_.fetch_add(1) + 1;
  pipeline_->queue().kick();
  return token;
}

void WorkerCore::prune_pass_results_locked() {
  // Oldest-first, bounded retention. Nothing else is safe: a completed pass may
  // still be read by a token that is not blocked on it right now.
  while (pass_results_.size() > kMaxRetainedPasses) {
    pass_results_.erase(pass_results_.begin());
  }
}

Status WorkerCore::flush_result_locked(std::uint64_t token) const {
  // EXACT match only. Sliding to a neighbouring entry would attribute another
  // flush's outcome to this token.
  const auto it = pass_results_.find(token);
  if (it != pass_results_.end()) {
    return it->second;
  }

  // No record — and the two reasons for that are entirely different truths.
  //
  // ABOVE every recorded token: the worker had already stopped when this flush
  // was submitted, so the pass NEVER RAN. Reporting Ok here manufactured a
  // success out of the worker's clean exit — a caller could see Ok from flush()
  // while durable events were still unsent. Nothing was verified, so nothing is
  // claimed: this is a terminal refusal.
  //
  // shutdown() submits its own best-effort drain and tolerates this code
  // specifically (see Client::Impl::shutdown), because there the stopped worker
  // is the intended outcome rather than a surprise.
  if (finished_.load() &&
      (pass_results_.empty() || token > pass_results_.rbegin()->first)) {
    return Status(ErrorCode::AlreadyShutdown,
                  "the worker stopped before this flush ran; nothing was "
                  "flushed and any pending events remain in durable storage");
  }

  // BELOW the retained window: this token WAS serviced and its result has since
  // been discarded to bound memory. Reporting Ok would be a false success, and
  // returning a neighbour's entry would be a lie about a different flush.
  return Status(ErrorCode::Internal,
                "the result for this flush (token " + std::to_string(token) +
                    ") is no longer available: it was discarded after more "
                    "than " +
                    std::to_string(kMaxRetainedPasses) +
                    " later flushes completed. Submit a new flush to obtain a "
                    "current result.");
}

bool WorkerCore::wait_flush(std::uint64_t token,
                            std::chrono::milliseconds timeout, Status* out) {
  std::unique_lock<std::mutex> lock(mutex_);
  // Also wake if the worker has exited: a flush submitted against a worker that
  // is already finishing must not wait out its whole timeout for a token that
  // can never be completed.
  const bool done = cv_.wait_for(lock, timeout, [&] {
    return flush_done_.load() >= token || finished_.load();
  });
  if (!done) {
    return false;
  }
  if (out != nullptr) {
    if (!terminal_status_.ok()) {
      // An unexpected worker exit outranks whatever any pass recorded.
      *out = terminal_status_;
    } else {
      *out = flush_result_locked(token);
    }
  }
  prune_pass_results_locked();
  return true;
}

bool WorkerCore::wait_finished(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return cv_.wait_for(lock, timeout, [&] { return finished_.load(); });
}

Status WorkerCore::terminal_status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return terminal_status_;
}

void WorkerCore::forget_errors() {
  std::lock_guard<std::mutex> lock(mutex_);
  pending_error_ = Status();
}

void WorkerCore::discard_in_flight() {
  std::lock_guard<std::mutex> lock(retry_mutex_);
  const std::size_t dropped = retry_.size();
  retry_.clear();
  if (dropped > 0) {
    pipeline_->stats().discarded_consent.fetch_add(dropped);
    pipeline_->release_outstanding(dropped);
  }
}

void WorkerCore::note_error(const Status& status) {
  pipeline_->report_error(status);
  std::lock_guard<std::mutex> lock(mutex_);
  if (pending_error_.ok()) {
    pending_error_ = status;
  }
}

std::uint64_t WorkerCore::retry_front_ordinal() {
  std::lock_guard<std::mutex> lock(retry_mutex_);
  return retry_.empty() ? kNoOrdinal : retry_.front().ordinal;
}

void WorkerCore::take_retry(std::vector<QueuedEvent>* out) {
  std::lock_guard<std::mutex> lock(retry_mutex_);
  out->swap(retry_);
  retry_.clear();
}

void WorkerCore::hold_for_retry(std::vector<QueuedEvent>&& batch) {
  if (batch.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(retry_mutex_);
  // Retries stay ahead of anything newer so ordering survives a storage fault.
  if (retry_.empty()) {
    retry_ = std::move(batch);
  } else {
    batch.insert(batch.end(), std::make_move_iterator(retry_.begin()),
                 std::make_move_iterator(retry_.end()));
    retry_ = std::move(batch);
  }
}

void WorkerCore::run() {
  // Declared outside the try so the exit path below can tell which tokens were
  // actually serviced and which the worker never reached.
  std::uint64_t handled_flush = 0;
  // Outermost containment barrier: nothing may escape the thread.
  try {
    pipeline_->log(LogLevel::Info, "playertrace worker started");
    while (true) {
      // Latch the flush request BEFORE doing any work, and scope the error
      // slot to this pass. An error recorded before this flush was requested
      // may already have been resolved by a successful retry; charging it to
      // an unrelated later flush reported failure for data that was durable.
      // Anything genuinely still outstanding is caught by the residual check
      // below, and anything that fails while this pass drains is recorded here.
      const std::uint64_t want = flush_seq_.load();
      const bool flush_pending = want > handled_flush;
      if (flush_pending) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_error_ = Status();
      }

      std::vector<QueuedEvent> drained;
      // Never block while a flush is already waiting to be serviced.
      pipeline_->queue().pop_wait(pipeline_->config().batch_size,
                                  flush_pending
                                      ? std::chrono::milliseconds(0)
                                      : pipeline_->config().batch_interval,
                                  &drained);

      process_incoming(drained);
      deliver_all();

      if (flush_pending) {
        const std::uint64_t barrier = flush_barrier_.load();
        // Keep draining and delivering until nothing admitted at or below the
        // barrier remains anywhere, or until no further progress is possible.
        // There is deliberately no fixed iteration cap: a bound on batches is
        // not a bound on the work a caller actually accepted before flushing.
        Status why;
        while (work_remains_at_or_below(barrier, &why)) {
          const std::size_t queue_before = pipeline_->queue().size();

          std::vector<QueuedEvent> more;
          pipeline_->queue().pop_wait(pipeline_->config().batch_size,
                                      std::chrono::milliseconds(0), &more);
          const bool moved = !more.empty();
          process_incoming(more);
          const bool delivered = deliver_all();

          if (!moved && !delivered &&
              pipeline_->queue().size() >= queue_before) {
            break;  // stalled on a failing sink or store; reported below
          }
        }

        const Status sink_flush = safe_flush_sink();
        if (!sink_flush.ok()) {
          note_error(sink_flush);
        }

        // Report Ok only when nothing from before the barrier is left.
        Status outcome;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          outcome = pending_error_;
          pending_error_ = Status();
        }
        Status residual;
        if (work_remains_at_or_below(barrier, &residual) && outcome.ok()) {
          outcome = residual;
        }
        const std::uint64_t first_covered = handled_flush + 1;
        handled_flush = want;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          // Record this outcome for EVERY token the pass answered. Tokens
          // coalesce — one pass can satisfy several — and lookup is by exact
          // key, so each of them needs its own entry. Keying only by `want`
          // forced a nearest-match search, which slid onto an unrelated token's
          // result once retention pruned the original.
          std::uint64_t t = first_covered;
          while (t <= want) {
            pass_results_[t] = outcome;
            ++t;
          }
          prune_pass_results_locked();
          flush_done_.store(want);
        }
        cv_.notify_all();
      }

      if (stop_.load()) {
        break;
      }
    }
    pipeline_->log(LogLevel::Info, "playertrace worker stopped");
  } catch (const std::exception& ex) {
    std::lock_guard<std::mutex> lock(mutex_);
    terminal_status_ =
        Status(ErrorCode::Internal,
               std::string("the playertrace worker stopped unexpectedly: ") +
                   ex.what());
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    terminal_status_ = Status(
        ErrorCode::Internal,
        "the playertrace worker stopped unexpectedly (unknown exception)");
  }

  const Status terminal = terminal_status();
  if (!terminal.ok()) {
    // Close admission permanently BEFORE anything else, including the user's
    // error callback: from here nothing will drain the queue, so no producer
    // may be told Ok again. flush()/shutdown() already surface
    // terminal_status_; this is what makes track()/start_session()/
    // end_session() tell the truth as well.
    pipeline_->set_terminated();
    // Reported outside the catch so a throwing callback cannot re-enter it.
    pipeline_->report_error(terminal);
  }

  // Release anyone waiting on a flush or on the worker's exit, even on failure.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    finished_.store(true);
    const std::uint64_t last = flush_seq_.load();

    // Answer every token that was submitted but never serviced, so no token
    // that was legitimately issued is left without a record. Lookup is by exact
    // key, and "no record" specifically means "expired" — leaving these blank
    // would make an unserviced token indistinguishable from a discarded result.
    //
    // These tokens get an explicit REFUSAL, not Ok. Their pass never ran, so
    // nothing about them was verified: recording success here would let a
    // public flush() report Ok while events it was asked to flush were still
    // sitting undelivered in durable storage. shutdown() tolerates this exact
    // code for its own best-effort drain, where a stopped worker is the
    // intended outcome (see Client::Impl::shutdown).
    const Status unserviced(
        ErrorCode::AlreadyShutdown,
        "the worker stopped before this flush ran; nothing was flushed and any "
        "pending events remain in durable storage");
    std::uint64_t t = handled_flush + 1;
    while (t <= last) {
      pass_results_[t] = unserviced;
      ++t;
    }
    prune_pass_results_locked();
    flush_done_.store(last);
  }
  cv_.notify_all();
}

bool WorkerCore::work_remains_at_or_below(std::uint64_t barrier, Status* why) {
  if (barrier == 0) {
    return false;
  }
  if (pipeline_->queue().front_ordinal() <= barrier) {
    if (why != nullptr) {
      *why = Status(ErrorCode::Timeout,
                    "flush did not drain every event accepted before it");
    }
    return true;
  }
  if (retry_front_ordinal() <= barrier) {
    if (why != nullptr) {
      *why = Status(ErrorCode::StorageError,
                    "events accepted before this flush could not be written to "
                    "durable storage and are awaiting retry");
    }
    return true;
  }
  std::size_t pending = 0;
  Status counted;
  {
    std::lock_guard<std::mutex> lock(pipeline_->store_mutex());
    counted = pipeline_->store().count_up_to(barrier, &pending);
  }
  if (!counted.ok()) {
    if (why != nullptr) {
      *why = counted;
    }
    return true;
  }
  if (pending > 0) {
    if (why != nullptr) {
      *why = Status(ErrorCode::SinkError,
                    "events accepted before this flush are persisted but were "
                    "not acknowledged by the sink");
    }
    return true;
  }
  return false;
}

void WorkerCore::process_incoming(std::vector<QueuedEvent>& drained) {
  // Anything held back by a previous storage failure goes first.
  std::vector<QueuedEvent> batch;
  take_retry(&batch);
  if (!drained.empty()) {
    batch.insert(batch.end(), std::make_move_iterator(drained.begin()),
                 std::make_move_iterator(drained.end()));
    drained.clear();
  }
  if (batch.empty()) {
    return;
  }

  const std::uint64_t generation = pipeline_->consent_generation();

  // Consent is not granted: discard the work and make sure nothing older
  // survives in durable storage.
  //
  // Events drained before a revocation are handled by the per-event generation
  // check below, NOT by a sticky "drop everything" flag: such a flag would also
  // destroy legitimate work admitted after a re-grant.
  if (pipeline_->consent() != ConsentState::Granted) {
    pipeline_->stats().discarded_consent.fetch_add(batch.size());
    pipeline_->release_outstanding(batch.size());
    batch.clear();
    purge_store();
    return;
  }

  std::vector<StoredRecord> records;
  records.reserve(batch.size());
  std::vector<QueuedEvent> keep;
  keep.reserve(batch.size());
  for (auto& qe : batch) {
    // Events admitted under an older consent generation are dropped, so a rapid
    // revoke/re-grant can never resurrect them.
    if (qe.consent_generation != generation) {
      pipeline_->stats().discarded_consent.fetch_add(1);
      pipeline_->release_outstanding(1);
      continue;
    }
    std::string payload;
    const Status s = serializer_.serialize(qe.event, &payload);
    if (!s.ok()) {
      // One unserializable event is dropped and reported; it must never stall
      // or crash the pipeline. Admission-time validation makes this rare.
      pipeline_->report_error(s);
      pipeline_->stats().dropped_invalid.fetch_add(1);
      pipeline_->release_outstanding(1);
      continue;
    }
    records.push_back(StoredRecord{qe.event.event_id, qe.event.session_id,
                                   qe.event.sequence, qe.ordinal,
                                   qe.event.timestamp_ms, std::move(payload)});
    keep.push_back(std::move(qe));
  }
  batch.clear();
  if (records.empty()) {
    return;
  }

  Status inserted;
  bool revoked_at_boundary = false;
  {
    std::lock_guard<std::mutex> lock(pipeline_->store_mutex());
    // FINAL consent boundary. Revocation purges under this same mutex, so
    // rechecking here is what stops a stale batch from landing in storage after
    // set_consent() has already reported the purge complete.
    if (pipeline_->consent() != ConsentState::Granted ||
        pipeline_->consent_generation() != generation) {
      revoked_at_boundary = true;
    } else {
      inserted = pipeline_->store().insert(records);
    }
  }
  if (revoked_at_boundary) {
    pipeline_->stats().discarded_consent.fetch_add(keep.size());
    pipeline_->release_outstanding(keep.size());
    return;
  }

  if (!inserted.ok()) {
    // Accepted events must not be destroyed by a storage failure. They are held
    // in the worker's own retry buffer — not pushed back into the bounded
    // queue, which would exceed its capacity — and stay counted in
    // `outstanding`, so the durable cap still applies.
    note_error(inserted);
    hold_for_retry(std::move(keep));
    return;
  }
  pipeline_->stats().persisted.fetch_add(records.size());
}

bool WorkerCore::deliver_all() {
  bool delivered_any = false;
  while (true) {
    if (pipeline_->consent() != ConsentState::Granted) {
      purge_store();
      return delivered_any;
    }

    FetchResult pending;
    {
      std::lock_guard<std::mutex> lock(pipeline_->store_mutex());
      pending =
          pipeline_->store().fetch_pending(pipeline_->config().batch_size);
    }
    if (pending.quarantined > 0) {
      // Those rows are gone for good; free the capacity they were holding.
      pipeline_->release_outstanding(pending.quarantined);
      pipeline_->stats().dropped_invalid.fetch_add(pending.quarantined);
    }
    if (!pending.status.ok()) {
      // Read failures and quarantine notices belong to the active flush too.
      note_error(pending.status);
    }
    if (pending.events.empty()) {
      return delivered_any;
    }

    const std::uint64_t generation = pipeline_->consent_generation();
    if (pipeline_->consent() != ConsentState::Granted) {
      purge_store();
      return delivered_any;
    }

    EventBatch batch;
    batch.events = std::move(pending.events);
    const Status written = safe_write(batch);

    if (!written.ok()) {
      note_error(written);
      pipeline_->stats().delivery_failures.fetch_add(1);
      return delivered_any;  // leave the events durable; retry later
    }

    // If consent was revoked while the sink was writing, the purge already
    // removed these rows; do not count them as delivered.
    if (pipeline_->consent() != ConsentState::Granted ||
        pipeline_->consent_generation() != generation) {
      purge_store();
      return delivered_any;
    }

    std::vector<std::string> ids;
    ids.reserve(batch.events.size());
    for (const auto& e : batch.events) {
      ids.push_back(e.event_id);
    }
    Status ack;
    std::size_t removed = 0;
    {
      std::lock_guard<std::mutex> lock(pipeline_->store_mutex());
      ack = pipeline_->store().acknowledge(ids, &removed);
    }
    if (!ack.ok()) {
      // Delivered but not removable. Report it against the flush and end the
      // pass: the events stay durable and may be delivered again.
      note_error(ack);
      return delivered_any;
    }
    pipeline_->stats().delivered.fetch_add(batch.events.size());
    pipeline_->release_outstanding(batch.events.size());
    delivered_any = true;
  }
}

void WorkerCore::purge_store() {
  std::size_t deleted = 0;
  Status purged;
  std::size_t remaining = 0;
  {
    std::lock_guard<std::mutex> lock(pipeline_->store_mutex());
    purged = pipeline_->store().purge_and_clear_revocation(&deleted);
    if (purged.ok()) {
      pipeline_->store().count(&remaining);
    }
  }
  if (!purged.ok()) {
    note_error(purged);
    return;
  }
  pipeline_->reset_outstanding(static_cast<std::uint64_t>(remaining));
}

Status WorkerCore::safe_flush_sink() {
  try {
    const std::shared_ptr<Sink>& sink = pipeline_->sink();
    if (!sink) {
      return Status();
    }
    return sink->flush();
  } catch (const std::exception& ex) {
    return Status(ErrorCode::SinkError,
                  std::string("sink flush threw: ") + ex.what());
  } catch (...) {
    return Status(ErrorCode::SinkError,
                  "sink flush threw an unknown exception");
  }
}

Status WorkerCore::safe_write(const EventBatch& batch) {
  try {
    const std::shared_ptr<Sink>& sink = pipeline_->sink();
    if (!sink) {
      return Status(ErrorCode::SinkError, "no sink configured");
    }
    return sink->write(batch);
  } catch (const std::exception& ex) {
    return Status(ErrorCode::SinkError,
                  std::string("sink threw: ") + ex.what());
  } catch (...) {
    return Status(ErrorCode::SinkError, "sink threw an unknown exception");
  }
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

Worker::~Worker() {
  // The thread is never abandoned: it can reach state captured by the user's
  // sink and callbacks, which this object does not own and cannot keep alive.
  // If a custom sink refuses to return, this blocks — documented honestly in
  // docs/reliability.md.
  core_->request_stop();
  join_blocking();
}

void Worker::start() {
  auto core = core_;  // the thread co-owns the core; it never sees `this`
  thread_ = std::thread([core] { core->run(); });
  thread_id_ = thread_.get_id();
  started_ = true;
}

bool Worker::wait_and_join(std::chrono::milliseconds timeout) {
  if (!started_) {
    return true;
  }
  if (timeout < std::chrono::milliseconds(0)) {
    timeout = std::chrono::milliseconds(0);
  }
  if (!core_->wait_finished(timeout)) {
    return false;  // still running; the caller keeps owning it
  }
  join_blocking();
  return true;
}

void Worker::join_blocking() {
  std::lock_guard<std::mutex> lock(join_mutex_);
  if (joined_ || !thread_.joinable()) {
    joined_ = true;
    return;
  }
  thread_.join();
  joined_ = true;
}

}  // namespace internal
}  // namespace playertrace
