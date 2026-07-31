// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Internal header — NOT installed and NOT part of the public API.
#ifndef PLAYERTRACE_INTERNAL_CLOCK_HPP
#define PLAYERTRACE_INTERNAL_CLOCK_HPP

#include <atomic>
#include <chrono>
#include <cstdint>

namespace playertrace {
namespace internal {

/// Abstract clock so tests can inject deterministic time without sleeping.
class Clock {
 public:
  virtual ~Clock() = default;
  /// Current UTC time in epoch milliseconds.
  virtual std::int64_t now_utc_millis() const = 0;
};

/// Production clock backed by the system wall clock (UTC).
class SystemClock : public Clock {
 public:
  std::int64_t now_utc_millis() const override {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
        .count();
  }
};

/// Test clock with an explicitly controlled value. Thread-safe.
class ManualClock : public Clock {
 public:
  explicit ManualClock(std::int64_t start_ms = 0) : now_(start_ms) {}
  std::int64_t now_utc_millis() const override { return now_.load(); }
  void set(std::int64_t ms) { now_.store(ms); }
  void advance(std::int64_t ms) { now_.fetch_add(ms); }

 private:
  std::atomic<std::int64_t> now_;
};

}  // namespace internal
}  // namespace playertrace

#endif  // PLAYERTRACE_INTERNAL_CLOCK_HPP
