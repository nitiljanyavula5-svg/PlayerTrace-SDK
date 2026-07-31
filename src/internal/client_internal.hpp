// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Internal header — NOT installed and NOT part of the public API. It exists so
// unit tests can inject a deterministic clock, inject storage faults, and read
// internal counters WITHOUT exposing a test seam or a metrics API through the
// public headers. Application code must never include this file.
#ifndef PLAYERTRACE_INTERNAL_CLIENT_INTERNAL_HPP
#define PLAYERTRACE_INTERNAL_CLIENT_INTERNAL_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "internal/clock.hpp"
#include "internal/sqlite_store.hpp"
#include "internal/stats.hpp"
#include "playertrace/client.hpp"
#include "playertrace/config.hpp"

namespace playertrace {

/// Friend of Client. Its members are defined in client.cpp.
struct ClientInternal {
  /// Builds a Client using an injected clock (the public Client::create uses a
  /// SystemClock). Returns the same CreateResult type as the public factory.
  static Client::CreateResult create(Config config,
                                     std::shared_ptr<internal::Clock> clock);

  /// Reads a snapshot of internal counters from a live client.
  static internal::StatsSnapshot stats(const Client& client);

  /// Installs a deterministic storage fault hook (pass nullptr to clear).
  static void set_store_fault_hook(Client& client, internal::FaultHook hook);

  /// True once the background worker has genuinely exited. The worker is never
  /// abandoned, so this is the authoritative "no further sink or callback can
  /// run" signal.
  static bool worker_finished(const Client& client);

  /// Number of events currently persisted and awaiting delivery.
  static std::size_t pending_in_store(Client& client);

  // --- flush token seam ----------------------------------------------------
  // Client::flush() submits a token and waits on it in one call, which makes
  // the "waiter reads its result only AFTER a later pass finished" ordering
  // impossible to drive from outside. Splitting the two halves lets a test
  // establish that ordering deterministically instead of racing for it.

  /// Submits a flush covering everything admitted so far; returns its token.
  static std::uint64_t submit_flush(Client& client);

  /// Waits for a previously submitted token and reports THAT token's outcome.
  static bool wait_flush(Client& client, std::uint64_t token,
                         std::chrono::milliseconds timeout, Status* out);
};

}  // namespace playertrace

#endif  // PLAYERTRACE_INTERNAL_CLIENT_INTERNAL_HPP
