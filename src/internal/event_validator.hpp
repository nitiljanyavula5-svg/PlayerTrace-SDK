// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Internal header — NOT installed and NOT part of the public API.
#ifndef PLAYERTRACE_INTERNAL_EVENT_VALIDATOR_HPP
#define PLAYERTRACE_INTERNAL_EVENT_VALIDATOR_HPP

#include <cstddef>
#include <string>

#include "playertrace/event.hpp"
#include "playertrace/result.hpp"

namespace playertrace {
namespace internal {

/// Pure validation of event names, player ids, and property maps. No I/O, no
/// locks, so it is trivially testable and safe to call from any thread. Rules:
///  - name: 1..max_name_length chars; first char [A-Za-z_]; rest
///  [A-Za-z0-9._-].
///  - property key: same rules as name; not a reserved key/prefix.
///  - property count: <= max_properties.
///  - duplicate keys: rejected.
///  - string value: <= max_string_length bytes AND well-formed UTF-8.
///  - double value: must be finite (NaN/Inf rejected).
///  - player id: <= max_player_id_length bytes AND well-formed UTF-8.
///
/// UTF-8 validation matters for more than tidiness: JSON serialization rejects
/// ill-formed UTF-8, so admitting it would surface as a failure deep inside the
/// background worker instead of as a Status returned from track().
class EventValidator {
 public:
  EventValidator(std::size_t max_name_length, std::size_t max_properties,
                 std::size_t max_string_length,
                 std::size_t max_player_id_length)
      : max_name_length_(max_name_length),
        max_properties_(max_properties),
        max_string_length_(max_string_length),
        max_player_id_length_(max_player_id_length) {}

  Status validate_name(const std::string& name) const;
  Status validate_properties(const Properties& properties) const;
  Status validate_player_id(const std::string& player_id) const;

  /// True if `key` is reserved (collides with a top-level field name or the
  /// "pt_" prefix reserved for the SDK).
  static bool is_reserved_key(const std::string& key);

  /// Strict UTF-8 well-formedness check. Rejects stray continuation bytes,
  /// truncated sequences, overlong encodings, UTF-16 surrogate halves, and
  /// code points above U+10FFFF. Embedded NUL (U+0000) is well-formed UTF-8 and
  /// is accepted; it is escaped as \u0000 by the serializer.
  static bool is_valid_utf8(const std::string& text);

 private:
  Status validate_identifier(const std::string& id,
                             ErrorCode empty_or_bad) const;

  std::size_t max_name_length_;
  std::size_t max_properties_;
  std::size_t max_string_length_;
  std::size_t max_player_id_length_;
};

}  // namespace internal
}  // namespace playertrace

#endif  // PLAYERTRACE_INTERNAL_EVENT_VALIDATOR_HPP
