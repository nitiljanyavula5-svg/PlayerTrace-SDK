// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#include "internal/json_serializer.hpp"

#include <cstddef>
#include <cstdio>
#include <ctime>
#include <exception>

#include <nlohmann/json.hpp>

namespace playertrace {
namespace internal {

using ordered_json = nlohmann::ordered_json;

std::string JsonSerializer::iso8601_utc(std::int64_t epoch_ms) {
  // Floor-divide so pre-epoch timestamps do not produce a negative millisecond
  // component or a time_t one second in the future.
  std::int64_t secs64 = epoch_ms / 1000;
  int millis = static_cast<int>(epoch_ms % 1000);
  if (millis < 0) {
    millis += 1000;
    secs64 -= 1;
  }
  const std::time_t secs = static_cast<std::time_t>(secs64);
  std::tm tm_utc{};
  bool ok = false;
#if defined(_WIN32)
  ok = (gmtime_s(&tm_utc, &secs) == 0);
#else
  ok = (gmtime_r(&secs, &tm_utc) != nullptr);
#endif
  if (!ok) {
    // Out of range for the platform's calendar conversion. Emit a well-formed
    // placeholder rather than reading an uninitialized tm.
    return std::string("1970-01-01T00:00:00.000Z");
  }
  // Bound every field BEFORE formatting.
  //
  // Two reasons, and neither is cosmetic. A calendar conversion can
  // legitimately yield a year outside four digits for an extreme epoch_ms,
  // which would emit a timestamp that is not ISO-8601 at all. And because
  // `struct tm` members are plain ints, a compiler that cannot see a range
  // assumes the full int range: seven unbounded %d conversions plus separators
  // can reach ~84 characters, which overflows this buffer. GCC 13 reports
  // exactly that as -Wformat-truncation, and the documented build uses -Werror.
  //
  // These explicit comparisons give value-range propagation the bounds it
  // needs, so the longest representable result is "9999-12-31T23:59:60.999Z" —
  // 24 characters plus the terminator. The warning is therefore answered with a
  // proof rather than suppressed with a pragma.
  const int year = tm_utc.tm_year + 1900;
  const int month = tm_utc.tm_mon + 1;
  const int day = tm_utc.tm_mday;
  const int hour = tm_utc.tm_hour;
  const int minute = tm_utc.tm_min;
  const int second = tm_utc.tm_sec;  // 60 is a valid leap second
  if (year < 0 || year > 9999 || month < 1 || month > 12 || day < 1 ||
      day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 60 || millis < 0 || millis > 999) {
    return std::string("1970-01-01T00:00:00.000Z");
  }

  char buf[40];
  const int written =
      std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                    year, month, day, hour, minute, second, millis);
  if (written < 0 || static_cast<std::size_t>(written) >= sizeof(buf)) {
    // Unreachable given the bounds above; never return a truncated timestamp.
    return std::string("1970-01-01T00:00:00.000Z");
  }
  return std::string(buf, static_cast<std::size_t>(written));
}

Status JsonSerializer::serialize(const Event& event, std::string* out) const {
  if (out == nullptr) {
    return Status(ErrorCode::Internal, "serialize: null output");
  }
  try {
    ordered_json j;
    j["event_id"] = event.event_id;
    j["app_id"] = event.app_id;
    j["name"] = event.name;
    j["schema_version"] = event.schema_version;
    j["timestamp_ms"] = event.timestamp_ms;
    j["timestamp"] = iso8601_utc(event.timestamp_ms);
    j["session_id"] = event.session_id;
    if (!event.player_id.empty()) {
      j["player_id"] = event.player_id;
    }
    j["seq"] = event.sequence;

    ordered_json props = ordered_json::object();
    for (const auto& kv : event.properties) {
      const std::string& key = kv.first;
      const PropertyValue& value = kv.second;
      switch (value.type()) {
        case PropertyValue::Type::Bool:
          props[key] = value.as_bool();
          break;
        case PropertyValue::Type::Int:
          props[key] = value.as_int();
          break;
        case PropertyValue::Type::Double:
          props[key] = value.as_double();
          break;
        case PropertyValue::Type::String:
          props[key] = value.as_string();
          break;
      }
    }
    j["properties"] = std::move(props);

    *out = j.dump();  // compact, no trailing newline
    return Status();
  } catch (const std::exception& ex) {
    out->clear();
    return Status(ErrorCode::Internal,
                  std::string("event serialization failed: ") + ex.what());
  } catch (...) {
    out->clear();
    return Status(ErrorCode::Internal,
                  "event serialization failed with an unknown exception");
  }
}

bool JsonSerializer::is_valid_json(const std::string& text) {
  try {
    return nlohmann::json::accept(text);
  } catch (...) {
    return false;
  }
}

}  // namespace internal
}  // namespace playertrace
