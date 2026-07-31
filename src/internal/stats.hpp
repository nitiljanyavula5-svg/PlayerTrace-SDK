// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Internal header — NOT installed and NOT part of the public API.
#ifndef PLAYERTRACE_INTERNAL_STATS_HPP
#define PLAYERTRACE_INTERNAL_STATS_HPP

#include <atomic>
#include <cstdint>

namespace playertrace {
namespace internal {

/// Internal counters for observability and testing. v0.1 does not expose a
/// public metrics API (see RFC §15); these are surfaced via the error callback
/// and read by tests through the internal test-access header.
struct Stats {
  std::atomic<std::uint64_t> accepted{0};  ///< track() returned Ok (Queued).
  std::atomic<std::uint64_t> dropped_queue_full{
      0};  ///< Rejected at admission: in-memory queue full.
  std::atomic<std::uint64_t> dropped_storage_full{
      0};  ///< Rejected at admission: durable cap reached.
  std::atomic<std::uint64_t> discarded_consent{
      0};  ///< Dropped due to consent revocation.
  std::atomic<std::uint64_t> dropped_invalid{
      0};  ///< Dropped because the event could not be serialized.
  std::atomic<std::uint64_t> persisted{0};  ///< Committed to SQLite.
  std::atomic<std::uint64_t> delivered{0};  ///< Acknowledged by a sink.
  std::atomic<std::uint64_t> delivery_failures{
      0};  ///< Sink write failures (batches).
};

/// A plain (non-atomic) snapshot for reading in tests.
struct StatsSnapshot {
  std::uint64_t accepted{0};
  std::uint64_t dropped_queue_full{0};
  std::uint64_t dropped_storage_full{0};
  std::uint64_t discarded_consent{0};
  std::uint64_t dropped_invalid{0};
  std::uint64_t persisted{0};
  std::uint64_t delivered{0};
  std::uint64_t delivery_failures{0};
  std::uint64_t outstanding{0};  ///< Admitted but not yet delivered/discarded.
};

}  // namespace internal
}  // namespace playertrace

#endif  // PLAYERTRACE_INTERNAL_STATS_HPP
