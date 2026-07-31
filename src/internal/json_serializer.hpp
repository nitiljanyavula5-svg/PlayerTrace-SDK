// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Internal header — NOT installed and NOT part of the public API. This is the
// ONLY place nlohmann/json is used; it never appears in any public header.
#ifndef PLAYERTRACE_INTERNAL_JSON_SERIALIZER_HPP
#define PLAYERTRACE_INTERNAL_JSON_SERIALIZER_HPP

#include <cstdint>
#include <string>

#include "playertrace/event.hpp"
#include "playertrace/result.hpp"

namespace playertrace {
namespace internal {

/// Serializes an Event to a single JSON object string (no trailing newline).
/// Field order and property insertion order are preserved for deterministic
/// output. See docs/event-schema.md for the wire format.
class JsonSerializer {
 public:
  /// Writes the serialized event to *out. Never throws: any failure inside the
  /// JSON library (for example ill-formed UTF-8 that slipped past validation)
  /// is converted into a non-ok Status, so the background worker can report and
  /// drop one bad event instead of terminating the host process.
  Status serialize(const Event& event, std::string* out) const;

  /// True if `text` parses as valid JSON (used to detect corrupted rows).
  /// Never throws.
  static bool is_valid_json(const std::string& text);

  /// Formats epoch milliseconds as an ISO-8601 UTC string, e.g.
  /// "2026-07-23T18:04:05.123Z". Never throws.
  static std::string iso8601_utc(std::int64_t epoch_ms);
};

}  // namespace internal
}  // namespace playertrace

#endif  // PLAYERTRACE_INTERNAL_JSON_SERIALIZER_HPP
