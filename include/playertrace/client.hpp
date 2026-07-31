// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef PLAYERTRACE_CLIENT_HPP
#define PLAYERTRACE_CLIENT_HPP

#include <chrono>
#include <memory>
#include <string>

#include "config.hpp"
#include "consent.hpp"
#include "event.hpp"
#include "event_builder.hpp"
#include "result.hpp"

namespace playertrace {

/// The main SDK entry point. Each Client is fully independent (no global
/// singleton); multiple instances may coexist. All public methods are safe to
/// call concurrently. Internal SQLite and JSON types are hidden behind a PIMPL,
/// so this header pulls in no third-party dependencies.
///
/// Exception guarantee: every public method reports failure through Status and
/// contains exceptions raised internally — including from storage, a sink, or a
/// callback you supply — so nothing escapes into your game loop. The single
/// documented exception is std::bad_alloc raised while constructing the Status
/// that would have been returned: recovering from that would itself require an
/// allocation. On a host built with exceptions disabled, an allocation failure
/// terminates the process as it would anywhere else in that build.
class Client {
 public:
  /// Result of Client::create(). `client` is non-null iff `status.ok()`.
  struct CreateResult {
    Status status;
    std::unique_ptr<Client> client;
    bool ok() const noexcept { return status.ok(); }
  };

  /// Validates the config, opens durable storage, and starts the background
  /// worker. On failure, `status` explains why and `client` is null.
  ///
  /// Failures that would otherwise throw — an unopenable database, a thread
  /// that cannot be created, an exhausted random source — are translated into a
  /// Status (see the class-level exception guarantee above).
  static CreateResult create(Config config);

  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(Client&&) = delete;

  /// Begins a new session with a fresh random session id and a reset sequence.
  /// If a session is already active it is ended first. Emits a "session_start"
  /// standard event (subject to consent).
  Status start_session();
  Status start_session(std::string anonymous_player_id);

  /// Ends the active session, emitting a "session_end" standard event with the
  /// session duration (subject to consent). Returns NotStarted if none active.
  Status end_session();

  /// Records a custom event. Non-blocking. A returned Status::ok() means the
  /// event was ACCEPTED into the in-memory queue (Queued) — not yet persisted.
  Status track(std::string event_name, Properties properties = {});
  Status track(const EventBuilder& builder);

  /// Blocks up to `timeout` while every event accepted BEFORE this call is
  /// persisted, delivered, and acknowledged. Use at checkpoints (level end,
  /// save, exit).
  ///
  /// Returns Ok only when no pre-flush event remains queued, retrying,
  /// persisted-unsent, or unacknowledged. Otherwise it reports why: Timeout,
  /// StorageError, or SinkError. A sink that is refusing writes therefore makes
  /// flush() report SinkError rather than a misleading Ok.
  Status flush(std::chrono::milliseconds timeout);

  /// Closes admission, asks the sink to cancel (Sink::request_cancel), drains
  /// what it can, and waits up to `timeout` for the worker to exit.
  ///
  /// Returns Ok only once the worker has actually stopped. If the timeout
  /// expires the client stays in a stopping state and Timeout is returned; the
  /// worker thread is still owned and still running — it is never abandoned.
  /// Call again, or call wait_for_worker_exit(), to observe the real state.
  /// Undelivered events remain safely in durable storage either way.
  ///
  /// Calling this from a sink or callback running on the worker thread does not
  /// deadlock: it closes admission, requests stop, and returns Internal without
  /// waiting (it cannot join itself).
  Status shutdown(std::chrono::milliseconds timeout);

  /// Waits up to `timeout` for the background worker to finish. Returns true
  /// only when the worker has genuinely exited, after which no further sink or
  /// callback invocation can occur and it is safe to destroy anything they
  /// captured. Returns false on timeout (the worker is still running).
  ///
  /// Returns true immediately if the worker has already stopped.
  bool wait_for_worker_exit(std::chrono::milliseconds timeout);

  /// Updates the consent state at runtime.
  ///
  /// Setting anything other than Granted rejects new events, ends the active
  /// session, discards queued events, and durably purges pending unsent events
  /// before returning. Revocation is fail-closed: if the purge or its durable
  /// marker cannot be committed, this returns StorageError, collection stays
  /// blocked, and the purge is retried on the next attempt and after a restart.
  /// Re-granting consent while a purge is still outstanding is refused.
  ///
  /// See docs/privacy.md.
  Status set_consent(ConsentState state);
  ConsentState consent() const;

 private:
  class Impl;
  explicit Client(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;

  // Grants the internal test-support factory access to the private constructor
  // and Impl (for injecting a deterministic clock and reading counters). Never
  // used by application code; declared only in an internal, uninstalled header.
  friend struct ClientInternal;
};

}  // namespace playertrace

#endif  // PLAYERTRACE_CLIENT_HPP
