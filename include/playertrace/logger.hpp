// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef PLAYERTRACE_LOGGER_HPP
#define PLAYERTRACE_LOGGER_HPP

#include <functional>
#include <string>

#include "result.hpp"

namespace playertrace {

enum class LogLevel { Debug = 0, Info, Warn, Error };

const char* to_string(LogLevel level) noexcept;

/// Diagnostic log callback.
///
/// Threading: invoked from the background worker thread, and from the thread
/// that calls Client::create() or Client::set_consent() for events raised
/// there. It is never invoked from track(). Implementations must be thread-safe
/// and must not block.
///
/// Reentrancy: callbacks must not call back into the Client that invoked them.
/// flush() and shutdown() explicitly refuse to run on the worker thread and
/// return ErrorCode::Internal if attempted; other entry points may deadlock.
using LogCallback = std::function<void(LogLevel, const std::string&)>;

/// Asynchronous error callback for failures that happen after a call has
/// already returned: sink failures, storage transaction errors, events dropped
/// because consent was revoked, and events that could not be serialized.
///
/// It is NOT used for conditions that track() reports synchronously (such as
/// QueueFull, StorageFull, ConsentDenied, or validation errors); those are
/// returned to the caller instead, so a single problem is never reported twice.
///
/// Threading and reentrancy: same contract as LogCallback above.
using ErrorCallback = std::function<void(const Status&)>;

}  // namespace playertrace

#endif  // PLAYERTRACE_LOGGER_HPP
