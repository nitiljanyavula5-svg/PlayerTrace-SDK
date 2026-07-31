// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Internal header — NOT installed and NOT part of the public API.
#ifndef PLAYERTRACE_INTERNAL_SESSION_MANAGER_HPP
#define PLAYERTRACE_INTERNAL_SESSION_MANAGER_HPP

#include <cstdint>
#include <mutex>
#include <string>

#include "internal/clock.hpp"
#include "internal/id_generator.hpp"

namespace playertrace {
namespace internal {

/// Owns the current session identity and the per-session sequence counter.
///
/// A single mutex serializes session transitions and sequence assignment, which
/// makes the ordering rules deterministic (see docs/architecture.md §Sessions):
/// every accepted event is stamped with exactly one session_id and a gap-free
/// (for accepted events) monotonic sequence number.
class SessionManager {
 public:
  SessionManager(IdGenerator* ids, Clock* clock) : ids_(ids), clock_(clock) {}

  /// Begins a new session. Returns the new session id. Resets the sequence to 0
  /// and records the session start time.
  std::string begin(std::string player_id);

  /// Marks the session inactive. Returns true if a session was active, and if
  /// so writes the session duration in milliseconds to *duration_ms.
  bool end(std::int64_t* duration_ms);

  /// If a session is active, writes its current duration (ms) to *duration_ms
  /// without ending it. Returns false if no session is active.
  bool peek_duration(std::int64_t* duration_ms) const;

  bool active() const;

  /// If a session is active, assigns the next sequence number and copies the
  /// current session identity. Returns false if no session is active.
  bool next(std::string* session_id, std::string* player_id,
            std::uint64_t* sequence);

  /// Gives back a sequence number that was assigned by next() but whose event
  /// was never admitted, so accepted events stay gap-free. Only the most recent
  /// assignment can be returned, and only while the same session is active;
  /// callers hold the admission lock, which serializes producers.
  void rollback(const std::string& session_id, std::uint64_t sequence);

  /// A complete copy of the session state, used to undo a transition whose
  /// marker event could not be admitted. Committing session state before
  /// knowing the marker was accepted produced markerless sessions and
  /// session_end events with no matching session_start.
  struct Snapshot {
    bool active = false;
    std::string session_id;
    std::string player_id;
    std::uint64_t sequence = 0;
    std::int64_t started_ms = 0;
  };

  Snapshot save() const;
  void restore(const Snapshot& snapshot);

  /// Ends the session without emitting anything. Used by consent revocation,
  /// which must not leave an active session behind but also must not collect a
  /// session_end for data it just purged.
  void deactivate();

 private:
  IdGenerator* ids_;
  Clock* clock_;
  mutable std::mutex mutex_;
  bool active_{false};
  std::string session_id_;
  std::string player_id_;
  std::uint64_t sequence_{0};
  std::int64_t started_ms_{0};
};

}  // namespace internal
}  // namespace playertrace

#endif  // PLAYERTRACE_INTERNAL_SESSION_MANAGER_HPP
