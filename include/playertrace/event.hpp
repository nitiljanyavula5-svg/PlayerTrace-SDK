// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef PLAYERTRACE_EVENT_HPP
#define PLAYERTRACE_EVENT_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace playertrace {

/// Current event schema version stamped on every event.
constexpr int kEventSchemaVersion = 1;

/// A typed, flat property value. Exactly one of four supported types:
/// bool, int64_t, double, or std::string. No nested objects or arrays — the
/// property map is intentionally flat to keep downstream analytics simple.
class PropertyValue {
 public:
  enum class Type { Bool, Int, Double, String };

  PropertyValue(bool value) : type_(Type::Bool), bool_(value) {}
  PropertyValue(std::int64_t value) : type_(Type::Int), int_(value) {}
  // Convenience for plain `int` literals (e.g. {"deaths", 3}); stored as Int.
  PropertyValue(int value) : type_(Type::Int), int_(value) {}
  PropertyValue(double value) : type_(Type::Double), double_(value) {}
  PropertyValue(const char* value)
      : type_(Type::String), string_(value ? value : "") {}
  PropertyValue(std::string value)
      : type_(Type::String), string_(std::move(value)) {}

  Type type() const noexcept { return type_; }

  // Typed accessors. Precondition: type() matches the accessor.
  bool as_bool() const noexcept { return bool_; }
  std::int64_t as_int() const noexcept { return int_; }
  double as_double() const noexcept { return double_; }
  const std::string& as_string() const noexcept { return string_; }

 private:
  Type type_;
  bool bool_{false};
  std::int64_t int_{0};
  double double_{0.0};
  std::string string_;
};

/// Ordered key/value pair. Insertion order is preserved in serialization, and
/// duplicate keys are rejected by validation (see EventValidator).
using Property = std::pair<std::string, PropertyValue>;
using Properties = std::vector<Property>;

/// The full event model as recorded by the SDK. Developers do not construct
/// this directly for tracking (they call Client::track with a name +
/// Properties); it is exposed as a value type so the schema is documented in
/// code and so custom tooling can reason about it. Identifiers are random UUIDs
/// and are never derived from player, device, or hardware information.
struct Event {
  std::string event_id;  ///< Random UUIDv4; downstream dedup key.
  std::string app_id;    ///< From Config::app_id.
  std::string name;      ///< Developer-supplied event name.
  int schema_version{kEventSchemaVersion};
  std::int64_t timestamp_ms{0};  ///< UTC epoch milliseconds.
  std::string session_id;        ///< Random UUIDv4 for the session.
  std::string player_id;         ///< Optional developer-supplied anon id.
  std::uint64_t sequence{0};     ///< Per-session monotonic sequence.
  Properties properties;
};

}  // namespace playertrace

#endif  // PLAYERTRACE_EVENT_HPP
