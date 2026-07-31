// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef PLAYERTRACE_RESULT_HPP
#define PLAYERTRACE_RESULT_HPP

#include <string>
#include <utility>

namespace playertrace {

/// Stable, enumerated error codes. `message()` carries the human-readable
/// detail.
///
/// Note on `Ok` for track(): a track() call returning Ok means the event was
/// ACCEPTED into the in-memory queue (Queued) — it is NOT yet durably
/// persisted. At-least-once delivery begins only after the worker commits it to
/// storage.
enum class ErrorCode {
  Ok = 0,
  InvalidConfig,      ///< Config failed validation in Client::create().
  InvalidEventName,   ///< Event name empty, too long, or bad characters.
  InvalidProperty,    ///< A property value was unsupported or non-finite.
  ReservedKey,        ///< A property key collides with a reserved name/prefix.
  ValueTooLarge,      ///< A string value exceeded the configured limit.
  TooManyProperties,  ///< More properties than the configured maximum.
  DuplicateKey,       ///< The same property key appeared more than once.
  QueueFull,          ///< In-memory queue at capacity; newest event rejected.
  StorageFull,        ///< Durable pending count at cap; newest event rejected.
  ConsentDenied,      ///< Consent is not Granted; event never created.
  NotStarted,         ///< No active session, or SDK not started.
  AlreadyShutdown,    ///< Operation issued after shutdown().
  StorageError,       ///< SQLite-level failure (open, transaction, schema).
  SinkError,          ///< A sink failed or threw while writing a batch.
  Timeout,            ///< flush() did not complete within the timeout.
  Internal            ///< Unexpected internal error.
};

/// Human-readable, stable identifier for an ErrorCode (for logs, not parsing).
const char* to_string(ErrorCode code) noexcept;

/// Lightweight, exception-free status object returned across the public API.
class Status {
 public:
  Status() noexcept : code_(ErrorCode::Ok) {}
  Status(ErrorCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  bool ok() const noexcept { return code_ == ErrorCode::Ok; }
  ErrorCode code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }

  explicit operator bool() const noexcept { return ok(); }

 private:
  ErrorCode code_;
  std::string message_;
};

/// Minimal value-or-status carrier. Used sparingly; the public API mostly
/// returns Status directly, and Client::create() returns its own CreateResult.
template <class T>
class Result {
 public:
  Result(T value) : status_(), value_(std::move(value)), has_value_(true) {}
  Result(Status status) : status_(std::move(status)), has_value_(false) {}

  bool ok() const noexcept { return status_.ok() && has_value_; }
  const Status& status() const noexcept { return status_; }

  T& value() { return value_; }
  const T& value() const { return value_; }

 private:
  Status status_;
  T value_{};
  bool has_value_{false};
};

}  // namespace playertrace

#endif  // PLAYERTRACE_RESULT_HPP
