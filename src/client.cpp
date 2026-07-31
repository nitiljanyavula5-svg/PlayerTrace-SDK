// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#include "playertrace/client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "internal/client_internal.hpp"
#include "internal/clock.hpp"
#include "internal/event_queue.hpp"
#include "internal/event_validator.hpp"
#include "internal/id_generator.hpp"
#include "internal/path_registry.hpp"
#include "internal/pipeline.hpp"
#include "internal/session_manager.hpp"
#include "internal/sqlite_store.hpp"
#include "internal/stats.hpp"
#include "internal/worker.hpp"
#include "playertrace/file_sink.hpp"

namespace playertrace {

using internal::Clock;
using internal::EventValidator;
using internal::IdGenerator;
using internal::LifecycleState;
using internal::PathClaim;
using internal::PathKind;
using internal::Pipeline;
using internal::QueuedEvent;
using internal::SessionManager;
using internal::SqliteStore;
using internal::StatsSnapshot;
using internal::SystemClock;
using internal::Worker;

namespace {

/// Milliseconds left before `deadline`, never negative.
std::chrono::milliseconds remaining(
    std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (deadline <= now) {
    return std::chrono::milliseconds(0);
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

}  // namespace

// ---------------------------------------------------------------------------
// Client::Impl
//
// Admission (lifecycle + consent + session + capacity + sequence + enqueue) is
// one linearized step under `admission_mutex_`. Consent transitions are
// additionally serialized by `consent_mutex_`, so a re-grant can never overtake
// an unfinished revocation.
// ---------------------------------------------------------------------------
class Client::Impl {
 public:
  Impl(Config config, std::shared_ptr<Clock> clock,
       std::unique_ptr<SqliteStore> store, PathClaim storage_claim,
       std::uint64_t initial_outstanding, std::uint64_t initial_ordinal)
      : clock_(std::move(clock)),
        validator_(config.max_name_length, config.max_properties,
                   config.max_string_length, config.max_player_id_length),
        session_(&id_gen_, clock_.get()),
        max_pending_events_(config.max_pending_events),
        storage_claim_(std::move(storage_claim)),
        pipeline_(
            std::make_shared<Pipeline>(std::move(config), std::move(store))),
        // Continue numbering above anything recovered from a previous run so
        // delivery order stays consistent across a restart.
        ordinal_(initial_ordinal) {
    pipeline_->reset_outstanding(initial_outstanding);
    worker_ = std::make_unique<Worker>(pipeline_);
    worker_->start();

    const std::string& mode = pipeline_->store().journal_mode();
    if (!mode.empty() && mode != "wal" && mode != "memory") {
      pipeline_->log(LogLevel::Warn,
                     "SQLite journal mode is '" + mode +
                         "'; WAL was requested. Durability may be weaker than "
                         "documented on this filesystem.");
    }
  }

  ~Impl() {
    // Bounded best-effort drain, then an unconditional join. The worker is
    // never abandoned: it can reach state captured by the user's sink and
    // callbacks. A sink that refuses to return will block here, which is
    // documented rather than worked around.
    shutdown(std::chrono::seconds(5));
    if (worker_) {
      worker_->request_stop();
      request_sink_cancel();
      worker_->join_blocking();
    }
  }

  Status start_session(std::string player_id) {
    Status id_status = validator_.validate_player_id(player_id);
    if (!id_status.ok()) {
      return id_status;
    }
    std::lock_guard<std::mutex> lock(admission_mutex_);
    if (lifecycle_ != LifecycleState::Running) {
      return Status(ErrorCode::AlreadyShutdown, "client is shut down");
    }
    // Do not open a session that cannot record anything: its session_start
    // could never be collected.
    if (pipeline_->consent() != ConsentState::Granted) {
      return Status(ErrorCode::ConsentDenied, "consent is not granted");
    }

    Status terminal = terminal_locked();
    if (!terminal.ok()) {
      return terminal;
    }

    // Replacing a live session emits TWO markers — session_end then
    // session_start — and an admitted event can NEVER be un-admitted. Reserve
    // capacity for both before admitting either, so the transition is
    // all-or-nothing: either the caller gets the new session, or the existing
    // one is left exactly as it was. Producers are serialized by
    // admission_mutex_ and only the worker frees capacity, so a reservation
    // that succeeds here cannot be invalidated before both are admitted.
    const bool replacing = session_.active();
    if (replacing) {
      Status reserved = reserve_locked(2);
      if (!reserved.ok()) {
        return reserved;
      }
    }

    // Session state is committed ONLY if its marker is admitted. Otherwise a
    // rejected marker left a markerless session behind, and later a session_end
    // with no matching session_start.
    const SessionManager::Snapshot previous = session_.save();

    if (replacing) {
      Status ended = end_session_locked();
      if (!ended.ok()) {
        // Nothing was admitted, so undoing is sound.
        session_.restore(previous);
        return ended;
      }
    }
    session_.begin(std::move(player_id));
    Status marker = admit_locked("session_start", Properties());
    if (!marker.ok()) {
      if (replacing) {
        // Unreachable given the reservation above. If it ever happens, the old
        // session's session_end HAS been admitted and cannot be withdrawn.
        // Restoring `previous` here would resurrect that session with its
        // sequence counter rewound, delivering events after their own
        // session_end and reusing (session_id, seq). Leave no active session.
        session_.deactivate();
      } else {
        session_.restore(previous);
      }
      return marker;
    }
    return Status();
  }

  Status end_session() {
    std::lock_guard<std::mutex> lock(admission_mutex_);
    if (lifecycle_ != LifecycleState::Running) {
      return Status(ErrorCode::AlreadyShutdown, "client is shut down");
    }
    // Checked ahead of the session test so a dead worker is reported as such
    // rather than as "no active session".
    Status terminal = terminal_locked();
    if (!terminal.ok()) {
      return terminal;
    }
    return end_session_locked();
  }

  Status track(std::string name, const Properties& properties) {
    // Remember which consent generation this call started under. Validation and
    // the user's property filter run outside the admission lock, so consent can
    // be revoked while we are here; such an in-flight call must not slip in
    // after the revocation boundary, even if consent is re-granted at once.
    const std::uint64_t entry_generation = pipeline_->consent_generation();

    // Validate the ORIGINAL input before the privacy filter runs, so a filter
    // can never mask invalid, duplicate, reserved, or oversized properties.
    Status name_status = validator_.validate_name(name);
    if (!name_status.ok()) {
      return name_status;
    }
    Status prop_status = validator_.validate_properties(properties);
    if (!prop_status.ok()) {
      return prop_status;
    }

    Properties filtered;
    apply_filter(properties, &filtered);

    std::lock_guard<std::mutex> lock(admission_mutex_);
    if (pipeline_->consent_generation() != entry_generation) {
      return Status(ErrorCode::ConsentDenied,
                    "consent was revoked while this event was being prepared");
    }
    return admit_locked(std::move(name), std::move(filtered));
  }

  Status flush(std::chrono::milliseconds timeout) {
    if (worker_ && worker_->is_current_thread()) {
      return Status(ErrorCode::Internal,
                    "flush() must not be called from a PlayerTrace callback or "
                    "sink running on the worker thread");
    }
    if (timeout < std::chrono::milliseconds(0)) {
      timeout = std::chrono::milliseconds(0);
    }

    std::uint64_t token = 0;
    {
      // The lifecycle check and the token issuance happen under ONE hold of the
      // admission lock — the same lock shutdown() takes to close admission.
      //
      // Splitting them was a race with a false-success outcome: shutdown could
      // move the client to Stopping and stop the worker between the check and
      // submit_flush(), leaving a token no pass would ever service while
      // durable events were still unsent. Only `Stopped` was rejected too, so a
      // flush issued during `Stopping` was accepted in the first place.
      std::lock_guard<std::mutex> lock(admission_mutex_);
      if (lifecycle_ != LifecycleState::Running) {
        return Status(ErrorCode::AlreadyShutdown,
                      "client is shutting down or shut down; flush() cannot "
                      "guarantee delivery of anything still pending");
      }
      Status terminal = terminal_locked();
      if (!terminal.ok()) {
        return terminal;
      }
      // Everything admitted so far is what this flush must cover. Captured
      // under the admission lock so no concurrent track() can slip below it.
      token = worker_->submit_flush(ordinal_);
    }
    Status outcome;
    if (!worker_->wait_flush(token, timeout, &outcome)) {
      return Status(ErrorCode::Timeout, "flush did not complete in time");
    }
    return outcome;
  }

  Status shutdown(std::chrono::milliseconds timeout) {
    if (worker_ && worker_->is_current_thread()) {
      // Never self-join. Close admission, ask the worker to stop, and say
      // truthfully that this call did not reach a terminal state.
      {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        if (lifecycle_ == LifecycleState::Running) {
          lifecycle_ = LifecycleState::Stopping;
        }
      }
      worker_->request_stop();
      return Status(ErrorCode::Internal,
                    "shutdown() was called from a PlayerTrace callback or sink "
                    "running on the worker thread; admission is now closed and "
                    "the worker was asked to stop, but this call cannot wait "
                    "for itself. Call shutdown() again from another thread.");
    }
    if (timeout < std::chrono::milliseconds(0)) {
      timeout = std::chrono::milliseconds(0);
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    {
      std::lock_guard<std::mutex> lock(admission_mutex_);
      if (lifecycle_ == LifecycleState::Stopped) {
        return Status();
      }
      // Closing admission first means no producer can be told "Ok" for work the
      // stopping worker will never process.
      lifecycle_ = LifecycleState::Stopping;
    }

    // Give the sink its cooperative chance to unblock.
    request_sink_cancel();

    // Submit the final flush BEFORE asking the worker to stop. Stopping first
    // let the worker observe the stop flag, exit, and never see the flush —
    // leaving this call waiting out its whole timeout on a token that could
    // never complete.
    Status persist_status;
    {
      std::uint64_t barrier = 0;
      {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        barrier = ordinal_;
      }
      const std::uint64_t token = worker_->submit_flush(barrier);
      // Reserve part of the budget for actually stopping. If the drain were
      // allowed to consume the whole timeout, shutdown would report Timeout
      // even when the worker was about to exit cleanly — the drain is
      // best-effort, but reaching a terminal state is the point of this call.
      worker_->wait_flush(token, remaining(deadline) / 2, &persist_status);
    }
    worker_->request_stop();

    // Stopped is reached ONLY when the worker has genuinely exited and been
    // joined. Every caller — concurrent or later — observes that same truth.
    const bool stopped = worker_->wait_and_join(remaining(deadline));
    if (!stopped) {
      return Status(ErrorCode::Timeout,
                    "shutdown timed out: the worker is still inside a sink or "
                    "callback and is still owned by this Client. It was NOT "
                    "abandoned. Call shutdown() or wait_for_worker_exit() "
                    "again; undelivered events remain in durable storage.");
    }
    {
      std::lock_guard<std::mutex> lock(admission_mutex_);
      lifecycle_ = LifecycleState::Stopped;
    }
    lifecycle_cv_.notify_all();

    Status terminal = worker_->terminal_status();
    if (!terminal.ok()) {
      return terminal;
    }
    // A sink that refused the final batch is NOT a shutdown failure: the events
    // stay in durable storage and go out on the next run, exactly as
    // documented. Only a storage-side failure — where data really is at risk —
    // is reported here.
    //
    // AlreadyShutdown from the drain means the worker had already stopped when
    // the drain was submitted, so that pass never ran. For a PUBLIC flush that
    // is a refusal — it verified nothing — but here the stopped worker is
    // exactly what this call set out to achieve, and the events stay durable.
    // Reaching a terminal state is what shutdown reports on; the drain is
    // best-effort by documented design.
    if (!persist_status.ok() && persist_status.code() != ErrorCode::SinkError &&
        persist_status.code() != ErrorCode::AlreadyShutdown) {
      return persist_status;
    }
    return Status();
  }

  bool wait_for_worker_exit(std::chrono::milliseconds timeout) {
    if (!worker_) {
      return true;
    }
    if (worker_->is_current_thread()) {
      return false;  // cannot wait for ourselves
    }
    if (timeout < std::chrono::milliseconds(0)) {
      timeout = std::chrono::milliseconds(0);
    }
    if (!worker_->wait_and_join(timeout)) {
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(admission_mutex_);
      if (lifecycle_ != LifecycleState::Running) {
        lifecycle_ = LifecycleState::Stopped;
      }
    }
    lifecycle_cv_.notify_all();
    return true;
  }

  Status set_consent(ConsentState state) {
    // One consent transition at a time: a re-grant must never overtake an
    // unfinished revocation, and two revocations must not interleave purges.
    std::lock_guard<std::mutex> consent_lock(consent_mutex_);

    if (state == ConsentState::Granted) {
      // Fail closed: never re-enable collection while a purge is still owed.
      if (purge_required_) {
        const Status retried = run_purge();
        if (!retried.ok()) {
          return Status(ErrorCode::StorageError,
                        "consent cannot be granted while a previous revocation "
                        "is still unfinished: " +
                            retried.message());
        }
      }
      std::lock_guard<std::mutex> lock(admission_mutex_);
      pipeline_->set_consent_state(state);
      return Status();
    }

    {
      std::lock_guard<std::mutex> lock(admission_mutex_);
      pipeline_->set_consent_state(state);
      // Invalidate everything admitted so far; a later re-grant starts a new
      // generation, so old work can never become valid again.
      pipeline_->bump_consent_generation();
      const std::size_t dropped = pipeline_->queue().clear();
      if (dropped > 0) {
        pipeline_->stats().discarded_consent.fetch_add(dropped);
        pipeline_->release_outstanding(dropped);
      }
      // Revocation must not leave an active session behind, and must not emit a
      // session_end for data it is about to purge.
      session_.deactivate();
      purge_required_ = true;
      if (worker_) {
        worker_->discard_in_flight();
        worker_->forget_errors();
      }
    }

    return run_purge();
  }

  ConsentState consent() const { return pipeline_->consent(); }

  StatsSnapshot snapshot_stats() const {
    const internal::Stats& s = pipeline_->stats();
    StatsSnapshot out;
    out.accepted = s.accepted.load();
    out.dropped_queue_full = s.dropped_queue_full.load();
    out.dropped_storage_full = s.dropped_storage_full.load();
    out.discarded_consent = s.discarded_consent.load();
    out.dropped_invalid = s.dropped_invalid.load();
    out.persisted = s.persisted.load();
    out.delivered = s.delivered.load();
    out.delivery_failures = s.delivery_failures.load();
    out.outstanding = pipeline_->outstanding();
    return out;
  }

  Pipeline& pipeline() { return *pipeline_; }
  bool worker_finished() const { return worker_ && worker_->finished(); }

  /// Test seam: the two halves of flush(), separately (see
  /// client_internal.hpp).
  std::uint64_t submit_flush_token() {
    std::uint64_t barrier = 0;
    {
      std::lock_guard<std::mutex> lock(admission_mutex_);
      barrier = ordinal_;
    }
    return worker_->submit_flush(barrier);
  }
  bool wait_flush_token(std::uint64_t token, std::chrono::milliseconds timeout,
                        Status* out) {
    return worker_->wait_flush(token, timeout, out);
  }

 private:
  void request_sink_cancel() {
    const std::shared_ptr<Sink>& sink = pipeline_->sink();
    if (!sink) {
      return;
    }
    // noexcept by contract, but a misbehaving override must not escape.
    try {
      sink->request_cancel();
    } catch (...) {
    }
  }

  /// The durable half of a revocation: mark, purge, clear. Two phases, so a
  /// crash between them still leaves proof that a purge is owed.
  Status run_purge() {
    Status marked;
    {
      std::lock_guard<std::mutex> lock(pipeline_->store_mutex());
      marked = pipeline_->store().mark_revocation_pending();
    }
    if (!marked.ok()) {
      pipeline_->report_error(marked);
      pipeline_->log(
          LogLevel::Error,
          "consent revocation could not record its durable marker: " +
              marked.message());
      return Status(
          ErrorCode::StorageError,
          "consent revocation could not be made durable: " + marked.message());
    }

    Status purged;
    std::size_t deleted = 0;
    {
      std::lock_guard<std::mutex> lock(pipeline_->store_mutex());
      purged = pipeline_->store().purge_and_clear_revocation(&deleted);
    }
    if (!purged.ok()) {
      // Fail closed. The marker stays committed, so a restart re-runs the
      // purge; accounting is deliberately NOT reset, because the rows remain.
      pipeline_->report_error(purged);
      pipeline_->log(LogLevel::Error,
                     "consent revocation could not purge durable storage: " +
                         purged.message());
      return Status(ErrorCode::StorageError,
                    "consent revocation could not purge durable storage; "
                    "collection stays blocked and the purge will be retried: " +
                        purged.message());
    }

    purge_required_ = false;
    pipeline_->reset_outstanding(0);
    return Status();
  }

  // Requires admission_mutex_.
  Status end_session_locked() {
    std::int64_t duration_ms = 0;
    if (!session_.peek_duration(&duration_ms)) {
      return Status(ErrorCode::NotStarted, "no active session");
    }
    Properties props;
    props.emplace_back("session_seconds",
                       static_cast<double>(duration_ms) / 1000.0);
    // Close the session ONLY if its marker was admitted; otherwise the session
    // would end with no session_end ever collected.
    Status marker = admit_locked("session_end", std::move(props));
    if (!marker.ok()) {
      return marker;
    }
    session_.end(nullptr);
    return Status();
  }

  // Requires admission_mutex_. Non-ok once the worker has exited for a reason
  // other than a stop request: nothing will drain the queue again, so admission
  // is closed for good and callers must be told the truth instead of receiving
  // a stale Ok for work no thread will ever process.
  Status terminal_locked() const {
    if (!pipeline_->terminated()) {
      return Status();
    }
    Status terminal = worker_ ? worker_->terminal_status() : Status();
    if (!terminal.ok()) {
      return terminal;
    }
    return Status(ErrorCode::Internal,
                  "the playertrace worker stopped unexpectedly; no further "
                  "events can be accepted");
  }

  // Requires admission_mutex_. Confirms that `count` events can ALL be admitted
  // right now, without admitting any of them. Used to make a multi-event
  // session transition atomic.
  Status reserve_locked(std::size_t count) {
    if (pipeline_->outstanding() + count > max_pending_events_) {
      pipeline_->stats().dropped_storage_full.fetch_add(1);
      return Status(ErrorCode::StorageFull,
                    "durable storage is full (max_pending_events=" +
                        std::to_string(max_pending_events_) +
                        "); event rejected");
    }
    if (!pipeline_->queue().has_room_for(count)) {
      pipeline_->stats().dropped_queue_full.fetch_add(1);
      return Status(ErrorCode::QueueFull,
                    "in-memory queue is full; event rejected");
    }
    return Status();
  }

  // Requires admission_mutex_. The whole admission decision as one step.
  Status admit_locked(std::string name, Properties properties) {
    // The terminal gate is checked first: a dead worker is a more specific
    // truth than "not running", and it is the only state nothing can leave.
    Status terminal = terminal_locked();
    if (!terminal.ok()) {
      return terminal;
    }
    if (lifecycle_ != LifecycleState::Running) {
      return Status(ErrorCode::AlreadyShutdown, "client is shut down");
    }
    if (pipeline_->consent() != ConsentState::Granted) {
      return Status(ErrorCode::ConsentDenied, "consent is not granted");
    }
    if (!session_.active()) {
      return Status(ErrorCode::NotStarted, "no active session");
    }
    // Durable capacity is enforced here so the caller learns synchronously.
    if (pipeline_->outstanding() >= max_pending_events_) {
      pipeline_->stats().dropped_storage_full.fetch_add(1);
      return Status(ErrorCode::StorageFull,
                    "durable storage is full (max_pending_events=" +
                        std::to_string(max_pending_events_) +
                        "); event rejected");
    }
    if (pipeline_->queue().full()) {
      pipeline_->stats().dropped_queue_full.fetch_add(1);
      return Status(ErrorCode::QueueFull,
                    "in-memory queue is full; event rejected");
    }

    QueuedEvent qe;
    if (!session_.next(&qe.event.session_id, &qe.event.player_id,
                       &qe.event.sequence)) {
      return Status(ErrorCode::NotStarted, "no active session");
    }
    qe.event.event_id = id_gen_.uuid4();
    qe.event.app_id = pipeline_->config().app_id;
    qe.event.name = std::move(name);
    qe.event.schema_version = kEventSchemaVersion;
    qe.event.timestamp_ms = clock_->now_utc_millis();
    qe.event.properties = std::move(properties);
    qe.ordinal = ++ordinal_;
    qe.consent_generation = pipeline_->consent_generation();

    const std::string session_id = qe.event.session_id;
    const std::uint64_t sequence = qe.event.sequence;

    // Reserve capacity BEFORE publishing to the queue. Publishing first let the
    // worker deliver and release the event before the increment landed, leaving
    // a phantom count that permanently consumed durable capacity.
    pipeline_->add_outstanding(1);
    if (!pipeline_->queue().try_push(std::move(qe))) {
      pipeline_->release_outstanding(1);
      session_.rollback(session_id, sequence);
      pipeline_->stats().dropped_queue_full.fetch_add(1);
      return Status(ErrorCode::QueueFull,
                    "in-memory queue is full; event rejected");
    }
    pipeline_->stats().accepted.fetch_add(1);
    return Status();  // Ok == Queued/Accepted (not yet persisted)
  }

  void apply_filter(const Properties& in, Properties* out) {
    const PropertyFilter& filter = pipeline_->config().property_filter;
    if (!filter) {
      *out = in;
      return;
    }
    out->clear();
    out->reserve(in.size());
    for (const auto& kv : in) {
      bool keep = true;
      try {
        keep = filter(kv.first, kv.second);
      } catch (...) {
        keep = false;  // a throwing filter drops the property, never crashes
      }
      if (keep) {
        out->push_back(kv);
      }
    }
  }

  std::shared_ptr<Clock> clock_;
  IdGenerator id_gen_;
  EventValidator validator_;
  SessionManager session_;
  std::size_t max_pending_events_;
  PathClaim storage_claim_;
  std::shared_ptr<Pipeline> pipeline_;

  mutable std::mutex admission_mutex_;
  std::condition_variable lifecycle_cv_;
  LifecycleState lifecycle_ = LifecycleState::Running;
  std::uint64_t ordinal_ = 0;

  std::mutex consent_mutex_;
  bool purge_required_ = false;

  std::unique_ptr<Worker> worker_;  // declared last -> destroyed first
};

// ---------------------------------------------------------------------------
// Client — thin forwarding shell. Each entry point contains exceptions so no
// internal failure escapes the public API.
// ---------------------------------------------------------------------------
Client::Client(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Client::~Client() = default;

Client::CreateResult Client::create(Config config) {
  return ClientInternal::create(std::move(config),
                                std::make_shared<SystemClock>());
}

Status Client::start_session() {
  try {
    return impl_->start_session(std::string());
  } catch (const std::exception& ex) {
    return Status(ErrorCode::Internal,
                  std::string("start_session failed: ") + ex.what());
  } catch (...) {
    return Status(ErrorCode::Internal, "start_session failed");
  }
}

Status Client::start_session(std::string anonymous_player_id) {
  try {
    return impl_->start_session(std::move(anonymous_player_id));
  } catch (const std::exception& ex) {
    return Status(ErrorCode::Internal,
                  std::string("start_session failed: ") + ex.what());
  } catch (...) {
    return Status(ErrorCode::Internal, "start_session failed");
  }
}

Status Client::end_session() {
  try {
    return impl_->end_session();
  } catch (const std::exception& ex) {
    return Status(ErrorCode::Internal,
                  std::string("end_session failed: ") + ex.what());
  } catch (...) {
    return Status(ErrorCode::Internal, "end_session failed");
  }
}

Status Client::track(std::string event_name, Properties properties) {
  try {
    return impl_->track(std::move(event_name), properties);
  } catch (const std::exception& ex) {
    return Status(ErrorCode::Internal,
                  std::string("track failed: ") + ex.what());
  } catch (...) {
    return Status(ErrorCode::Internal, "track failed");
  }
}

Status Client::track(const EventBuilder& builder) {
  try {
    return impl_->track(builder.name(), builder.properties());
  } catch (const std::exception& ex) {
    return Status(ErrorCode::Internal,
                  std::string("track failed: ") + ex.what());
  } catch (...) {
    return Status(ErrorCode::Internal, "track failed");
  }
}

Status Client::flush(std::chrono::milliseconds timeout) {
  try {
    return impl_->flush(timeout);
  } catch (const std::exception& ex) {
    return Status(ErrorCode::Internal,
                  std::string("flush failed: ") + ex.what());
  } catch (...) {
    return Status(ErrorCode::Internal, "flush failed");
  }
}

Status Client::shutdown(std::chrono::milliseconds timeout) {
  try {
    return impl_->shutdown(timeout);
  } catch (const std::exception& ex) {
    return Status(ErrorCode::Internal,
                  std::string("shutdown failed: ") + ex.what());
  } catch (...) {
    return Status(ErrorCode::Internal, "shutdown failed");
  }
}

bool Client::wait_for_worker_exit(std::chrono::milliseconds timeout) {
  try {
    return impl_->wait_for_worker_exit(timeout);
  } catch (...) {
    return false;
  }
}

Status Client::set_consent(ConsentState state) {
  try {
    return impl_->set_consent(state);
  } catch (const std::exception& ex) {
    return Status(ErrorCode::Internal,
                  std::string("set_consent failed: ") + ex.what());
  } catch (...) {
    return Status(ErrorCode::Internal, "set_consent failed");
  }
}

ConsentState Client::consent() const {
  try {
    return impl_->consent();
  } catch (...) {
    return ConsentState::Unknown;
  }
}

// ---------------------------------------------------------------------------
// ClientInternal — friend factory used by tests (see client_internal.hpp).
// ---------------------------------------------------------------------------
namespace {

// Multiplied in std::size_t, not unsigned int: a limit computed in a narrower
// type and then widened is how such a constant silently wraps on a 32-bit
// target.
constexpr std::size_t kMaxConfigCount = std::size_t{100} * 1000 * 1000;

Status validate_config(const Config& config) {
  if (config.app_id.empty()) {
    return Status(ErrorCode::InvalidConfig, "app_id must not be empty");
  }
  if (config.app_id.size() > 64) {
    return Status(ErrorCode::InvalidConfig, "app_id exceeds 64 characters");
  }
  for (char c : config.app_id) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    if (!ok) {
      return Status(ErrorCode::InvalidConfig,
                    "app_id contains an invalid character");
    }
  }
  if (config.storage_path.empty()) {
    return Status(ErrorCode::InvalidConfig, "storage_path must not be empty");
  }
  if (config.batch_size == 0) {
    return Status(ErrorCode::InvalidConfig, "batch_size must be >= 1");
  }
  if (config.max_queue_size == 0) {
    return Status(ErrorCode::InvalidConfig, "max_queue_size must be >= 1");
  }
  if (config.max_pending_events == 0) {
    return Status(ErrorCode::InvalidConfig, "max_pending_events must be >= 1");
  }
  if (config.max_name_length == 0) {
    return Status(ErrorCode::InvalidConfig, "max_name_length must be >= 1");
  }
  if (config.max_string_length == 0) {
    return Status(ErrorCode::InvalidConfig, "max_string_length must be >= 1");
  }
  if (config.max_player_id_length == 0) {
    return Status(ErrorCode::InvalidConfig,
                  "max_player_id_length must be >= 1");
  }
  // A non-positive interval would make the worker spin without ever blocking.
  if (config.batch_interval <= std::chrono::milliseconds(0)) {
    return Status(ErrorCode::InvalidConfig,
                  "batch_interval must be greater than zero");
  }
  const std::size_t counts[] = {
      config.max_queue_size,      config.batch_size,
      config.max_pending_events,  config.max_properties,
      config.max_name_length,     config.max_string_length,
      config.max_player_id_length};
  for (std::size_t value : counts) {
    if (value > kMaxConfigCount) {
      return Status(ErrorCode::InvalidConfig,
                    "a configuration limit exceeds the supported maximum of " +
                        std::to_string(kMaxConfigCount));
    }
  }
  if (config.batch_size > config.max_queue_size) {
    return Status(ErrorCode::InvalidConfig,
                  "batch_size must not exceed max_queue_size");
  }
  return Status();
}

}  // namespace

Client::CreateResult ClientInternal::create(Config config,
                                            std::shared_ptr<Clock> clock) {
  Client::CreateResult result;
  try {
    Status config_status = validate_config(config);
    if (!config_status.ok()) {
      result.status = config_status;
      return result;
    }
    if (!clock) {
      result.status =
          Status(ErrorCode::InvalidConfig, "clock must not be null");
      return result;
    }

    // Two clients sharing one database would keep independent capacity counts
    // and ordinal generators, deliver the same rows twice, and let one client's
    // consent purge delete the other's events.
    PathClaim storage_claim;
    std::string claim_error;
    if (!storage_claim.acquire(config.storage_path, PathKind::Storage,
                               &claim_error)) {
      result.status = Status(ErrorCode::InvalidConfig, claim_error);
      return result;
    }

    if (!config.sink) {
      auto file_sink =
          std::make_shared<FileSink>(config.app_id + "-events.ndjson");
      const Status sink_status = file_sink->failed();
      if (!sink_status.ok()) {
        result.status = sink_status;
        return result;
      }
      config.sink = std::move(file_sink);
    }

    const ConsentState initial_consent = config.consent;
    auto store = std::make_unique<SqliteStore>(config.storage_path);
    Status open_status = store->open();
    if (!open_status.ok()) {
      result.status = open_status;
      return result;
    }

    // A revocation that never finished — or a run that opens without granted
    // consent — must be resolved BEFORE anything can be delivered or granted.
    bool owed = false;
    Status owed_status = store->revocation_pending(&owed);
    if (!owed_status.ok()) {
      result.status = owed_status;
      return result;
    }
    if (owed || initial_consent != ConsentState::Granted) {
      std::size_t purged = 0;
      Status purge = store->purge_and_clear_revocation(&purged);
      if (!purge.ok()) {
        result.status =
            Status(ErrorCode::StorageError,
                   "recovered events could not be purged before opening "
                   "without granted consent: " +
                       purge.message());
        return result;
      }
    }

    // Events recovered from a previous run count against the durable cap...
    std::size_t recovered = 0;
    Status count_status = store->count(&recovered);
    if (!count_status.ok()) {
      result.status = count_status;
      return result;
    }
    // ...and new events must be ordered after them.
    std::uint64_t highest_ordinal = 0;
    Status ordinal_status = store->max_ordinal(&highest_ordinal);
    if (!ordinal_status.ok()) {
      result.status = ordinal_status;
      return result;
    }

    auto impl = std::make_unique<Client::Impl>(
        std::move(config), std::move(clock), std::move(store),
        std::move(storage_claim), static_cast<std::uint64_t>(recovered),
        highest_ordinal);
    result.client = std::unique_ptr<Client>(new Client(std::move(impl)));
    result.status = Status();
    return result;
  } catch (const std::exception& ex) {
    result.client.reset();
    result.status = Status(ErrorCode::Internal,
                           std::string("client creation failed: ") + ex.what());
    return result;
  } catch (...) {
    result.client.reset();
    result.status =
        Status(ErrorCode::Internal, "client creation failed unexpectedly");
    return result;
  }
}

StatsSnapshot ClientInternal::stats(const Client& client) {
  return client.impl_->snapshot_stats();
}

void ClientInternal::set_store_fault_hook(Client& client,
                                          internal::FaultHook hook) {
  Pipeline& pipeline = client.impl_->pipeline();
  std::lock_guard<std::mutex> lock(pipeline.store_mutex());
  pipeline.store().set_fault_hook(std::move(hook));
}

bool ClientInternal::worker_finished(const Client& client) {
  return client.impl_->worker_finished();
}

std::uint64_t ClientInternal::submit_flush(Client& client) {
  return client.impl_->submit_flush_token();
}

bool ClientInternal::wait_flush(Client& client, std::uint64_t token,
                                std::chrono::milliseconds timeout,
                                Status* out) {
  return client.impl_->wait_flush_token(token, timeout, out);
}

std::size_t ClientInternal::pending_in_store(Client& client) {
  Pipeline& pipeline = client.impl_->pipeline();
  std::lock_guard<std::mutex> lock(pipeline.store_mutex());
  std::size_t n = 0;
  if (!pipeline.store().count(&n).ok()) {
    return 0;
  }
  return n;
}

}  // namespace playertrace
