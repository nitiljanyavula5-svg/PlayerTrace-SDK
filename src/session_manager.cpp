// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#include "internal/session_manager.hpp"

#include <utility>

namespace playertrace {
namespace internal {

std::string SessionManager::begin(std::string player_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  session_id_ = ids_->uuid4();
  player_id_ = std::move(player_id);
  sequence_ = 0;
  started_ms_ = clock_->now_utc_millis();
  active_ = true;
  return session_id_;
}

bool SessionManager::end(std::int64_t* duration_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_) {
    return false;
  }
  if (duration_ms != nullptr) {
    *duration_ms = clock_->now_utc_millis() - started_ms_;
  }
  active_ = false;
  return true;
}

bool SessionManager::peek_duration(std::int64_t* duration_ms) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_) {
    return false;
  }
  if (duration_ms != nullptr) {
    *duration_ms = clock_->now_utc_millis() - started_ms_;
  }
  return true;
}

bool SessionManager::active() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_;
}

bool SessionManager::next(std::string* session_id, std::string* player_id,
                          std::uint64_t* sequence) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_) {
    return false;
  }
  *session_id = session_id_;
  *player_id = player_id_;
  *sequence = sequence_++;
  return true;
}

SessionManager::Snapshot SessionManager::save() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Snapshot s;
  s.active = active_;
  s.session_id = session_id_;
  s.player_id = player_id_;
  s.sequence = sequence_;
  s.started_ms = started_ms_;
  return s;
}

void SessionManager::restore(const Snapshot& snapshot) {
  std::lock_guard<std::mutex> lock(mutex_);
  active_ = snapshot.active;
  session_id_ = snapshot.session_id;
  player_id_ = snapshot.player_id;
  sequence_ = snapshot.sequence;
  started_ms_ = snapshot.started_ms;
}

void SessionManager::deactivate() {
  std::lock_guard<std::mutex> lock(mutex_);
  active_ = false;
}

void SessionManager::rollback(const std::string& session_id,
                              std::uint64_t sequence) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Only undo the most recent assignment, and only if the session has not been
  // replaced in the meantime.
  if (active_ && session_id_ == session_id && sequence_ == sequence + 1) {
    sequence_ = sequence;
  }
}

}  // namespace internal
}  // namespace playertrace
