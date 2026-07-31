// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#include "internal/event_validator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace playertrace {
namespace internal {

namespace {

bool is_first_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool is_rest_char(char c) {
  return is_first_char(c) || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

}  // namespace

bool EventValidator::is_valid_utf8(const std::string& text) {
  const auto* p = reinterpret_cast<const unsigned char*>(text.data());
  const std::size_t n = text.size();
  std::size_t i = 0;
  while (i < n) {
    const unsigned char c = p[i];
    if (c < 0x80u) {  // U+0000..U+007F (NUL is well-formed and allowed)
      i += 1;
      continue;
    }

    std::size_t extra = 0;
    std::uint32_t cp = 0;
    if ((c & 0xE0u) == 0xC0u) {  // 110xxxxx
      extra = 1;
      cp = c & 0x1Fu;
    } else if ((c & 0xF0u) == 0xE0u) {  // 1110xxxx
      extra = 2;
      cp = c & 0x0Fu;
    } else if ((c & 0xF8u) == 0xF0u) {  // 11110xxx
      extra = 3;
      cp = c & 0x07u;
    } else {
      return false;  // stray continuation byte or invalid lead (0xF8..0xFF)
    }

    if (i + extra >= n) {
      return false;  // truncated sequence
    }
    for (std::size_t k = 1; k <= extra; ++k) {
      const unsigned char cc = p[i + k];
      if ((cc & 0xC0u) != 0x80u) {
        return false;  // missing continuation byte
      }
      cp = (cp << 6) | (cc & 0x3Fu);
    }

    // Reject overlong encodings, surrogate halves, and out-of-range values.
    if (extra == 1 && cp < 0x80u)
      return false;
    if (extra == 2 && cp < 0x800u)
      return false;
    if (extra == 3 && cp < 0x10000u)
      return false;
    if (cp > 0x10FFFFu)
      return false;
    if (cp >= 0xD800u && cp <= 0xDFFFu)
      return false;

    i += extra + 1;
  }
  return true;
}

bool EventValidator::is_reserved_key(const std::string& key) {
  // Reserved top-level field names that would collide when flattened
  // downstream.
  static const std::unordered_set<std::string> kReserved = {
      "event_id",  "app_id",       "name",       "schema_version",
      "timestamp", "timestamp_ms", "session_id", "player_id",
      "seq",       "properties"};
  if (kReserved.count(key) != 0) {
    return true;
  }
  // The "pt_" prefix is reserved for future SDK-defined properties.
  return key.size() >= 3 && key[0] == 'p' && key[1] == 't' && key[2] == '_';
}

Status EventValidator::validate_identifier(const std::string& id,
                                           ErrorCode bad_code) const {
  if (id.empty()) {
    return Status(bad_code, "identifier must not be empty");
  }
  if (id.size() > max_name_length_) {
    return Status(bad_code, "identifier '" + id + "' exceeds max length of " +
                                std::to_string(max_name_length_));
  }
  if (!is_first_char(id.front())) {
    return Status(bad_code, "identifier '" + id +
                                "' must start with a letter or underscore");
  }
  for (char c : id) {
    if (!is_rest_char(c)) {
      return Status(bad_code,
                    "identifier '" + id + "' contains an invalid character");
    }
  }
  return Status();
}

Status EventValidator::validate_name(const std::string& name) const {
  return validate_identifier(name, ErrorCode::InvalidEventName);
}

Status EventValidator::validate_player_id(const std::string& player_id) const {
  // An empty player id means "no anonymous id supplied" and is always allowed.
  if (player_id.empty()) {
    return Status();
  }
  if (player_id.size() > max_player_id_length_) {
    return Status(ErrorCode::ValueTooLarge,
                  "player_id exceeds max length of " +
                      std::to_string(max_player_id_length_) + " bytes");
  }
  if (!is_valid_utf8(player_id)) {
    return Status(ErrorCode::InvalidProperty,
                  "player_id is not well-formed UTF-8");
  }
  return Status();
}

Status EventValidator::validate_properties(const Properties& properties) const {
  if (properties.size() > max_properties_) {
    return Status(ErrorCode::TooManyProperties,
                  "event has " + std::to_string(properties.size()) +
                      " properties; max is " + std::to_string(max_properties_));
  }

  std::unordered_set<std::string> seen;
  seen.reserve(properties.size());

  for (const auto& kv : properties) {
    const std::string& key = kv.first;
    const PropertyValue& value = kv.second;

    Status name_status = validate_identifier(key, ErrorCode::InvalidProperty);
    if (!name_status.ok()) {
      return name_status;
    }
    if (is_reserved_key(key)) {
      return Status(ErrorCode::ReservedKey,
                    "property key '" + key + "' is reserved");
    }
    if (!seen.insert(key).second) {
      return Status(ErrorCode::DuplicateKey,
                    "duplicate property key '" + key + "'");
    }

    switch (value.type()) {
      case PropertyValue::Type::String:
        if (value.as_string().size() > max_string_length_) {
          return Status(ErrorCode::ValueTooLarge,
                        "string value for '" + key +
                            "' exceeds max length of " +
                            std::to_string(max_string_length_));
        }
        if (!is_valid_utf8(value.as_string())) {
          return Status(
              ErrorCode::InvalidProperty,
              "string value for '" + key + "' is not well-formed UTF-8");
        }
        break;
      case PropertyValue::Type::Double:
        if (!std::isfinite(value.as_double())) {
          return Status(ErrorCode::InvalidProperty,
                        "double value for '" + key + "' must be finite");
        }
        break;
      case PropertyValue::Type::Bool:
      case PropertyValue::Type::Int:
        break;
    }
  }
  return Status();
}

}  // namespace internal
}  // namespace playertrace
