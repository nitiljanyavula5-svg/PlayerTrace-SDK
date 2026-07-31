// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Out-of-line definitions for the small value-type helpers: enum-to-string
// converters and the version accessor.
#include "playertrace/consent.hpp"
#include "playertrace/logger.hpp"
#include "playertrace/result.hpp"
#include "playertrace/version.hpp"

namespace playertrace {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return "Ok";
    case ErrorCode::InvalidConfig:
      return "InvalidConfig";
    case ErrorCode::InvalidEventName:
      return "InvalidEventName";
    case ErrorCode::InvalidProperty:
      return "InvalidProperty";
    case ErrorCode::ReservedKey:
      return "ReservedKey";
    case ErrorCode::ValueTooLarge:
      return "ValueTooLarge";
    case ErrorCode::TooManyProperties:
      return "TooManyProperties";
    case ErrorCode::DuplicateKey:
      return "DuplicateKey";
    case ErrorCode::QueueFull:
      return "QueueFull";
    case ErrorCode::StorageFull:
      return "StorageFull";
    case ErrorCode::ConsentDenied:
      return "ConsentDenied";
    case ErrorCode::NotStarted:
      return "NotStarted";
    case ErrorCode::AlreadyShutdown:
      return "AlreadyShutdown";
    case ErrorCode::StorageError:
      return "StorageError";
    case ErrorCode::SinkError:
      return "SinkError";
    case ErrorCode::Timeout:
      return "Timeout";
    case ErrorCode::Internal:
      return "Internal";
  }
  return "Unknown";
}

const char* to_string(ConsentState state) noexcept {
  switch (state) {
    case ConsentState::Unknown:
      return "Unknown";
    case ConsentState::Denied:
      return "Denied";
    case ConsentState::Granted:
      return "Granted";
  }
  return "Unknown";
}

const char* to_string(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Debug:
      return "Debug";
    case LogLevel::Info:
      return "Info";
    case LogLevel::Warn:
      return "Warn";
    case LogLevel::Error:
      return "Error";
  }
  return "Unknown";
}

const char* version() noexcept {
  return PLAYERTRACE_VERSION_STRING;
}

}  // namespace playertrace
