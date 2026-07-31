// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_amalgamated.hpp>

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "internal/json_serializer.hpp"
#include "playertrace/event.hpp"

using playertrace::ErrorCode;
using playertrace::Event;
using playertrace::internal::JsonSerializer;

namespace {
Event make_event() {
  Event e;
  e.event_id = "11111111-2222-4333-8444-555555555555";
  e.app_id = "forest-adventure";
  e.name = "level_completed";
  e.schema_version = 1;
  e.timestamp_ms = 1'700'000'000'123;  // fixed for determinism
  e.session_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
  e.player_id = "anon-42";
  e.sequence = 7;
  e.properties = {
      {"level_id", std::string("forest_01")},
      {"deaths", std::int64_t{3}},
      {"completion_seconds", 183.7},
      {"perfect", false},
  };
  return e;
}

std::string serialize_ok(const Event& e) {
  JsonSerializer serializer;
  std::string out;
  const auto status = serializer.serialize(e, &out);
  REQUIRE(status.ok());
  return out;
}
}  // namespace

TEST_CASE("Serialized event round-trips all fields", "[serialization]") {
  const std::string text = serialize_ok(make_event());

  // No trailing newline on the serialized event.
  REQUIRE_FALSE(text.empty());
  CHECK(text.back() != '\n');

  auto j = nlohmann::json::parse(text);
  CHECK(j.at("event_id") == "11111111-2222-4333-8444-555555555555");
  CHECK(j.at("app_id") == "forest-adventure");
  CHECK(j.at("name") == "level_completed");
  CHECK(j.at("schema_version") == 1);
  CHECK(j.at("timestamp_ms") == 1'700'000'000'123LL);
  CHECK(j.at("session_id") == "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
  CHECK(j.at("player_id") == "anon-42");
  CHECK(j.at("seq") == 7);

  const auto& props = j.at("properties");
  CHECK(props.at("level_id") == "forest_01");
  CHECK(props.at("deaths") == 3);
  CHECK(props.at("completion_seconds") == Catch::Approx(183.7));
  CHECK(props.at("perfect") == false);
}

TEST_CASE("Property insertion order is preserved", "[serialization]") {
  const std::string text = serialize_ok(make_event());
  const auto level = text.find("level_id");
  const auto deaths = text.find("deaths");
  const auto seconds = text.find("completion_seconds");
  const auto perfect = text.find("perfect");
  CHECK(level < deaths);
  CHECK(deaths < seconds);
  CHECK(seconds < perfect);
}

TEST_CASE("player_id is omitted when empty", "[serialization]") {
  Event e = make_event();
  e.player_id.clear();
  auto j = nlohmann::json::parse(serialize_ok(e));
  CHECK_FALSE(j.contains("player_id"));
}

TEST_CASE("Timestamps format as ISO-8601 UTC", "[serialization]") {
  const std::string iso = JsonSerializer::iso8601_utc(1'700'000'000'123);
  // 1700000000123 ms == 2023-11-14T22:13:20.123Z
  CHECK(iso == "2023-11-14T22:13:20.123Z");
}

TEST_CASE("Pre-epoch timestamps do not produce negative components",
          "[serialization]") {
  // -1500 ms == 1969-12-31T23:59:58.500Z; the millisecond field must not be
  // negative and the second must borrow correctly.
  const std::string iso = JsonSerializer::iso8601_utc(-1500);
  CHECK(iso == "1969-12-31T23:59:58.500Z");
}

TEST_CASE("is_valid_json detects malformed payloads", "[serialization]") {
  CHECK(JsonSerializer::is_valid_json("{\"a\":1}"));
  CHECK_FALSE(JsonSerializer::is_valid_json("{not json"));
  CHECK_FALSE(JsonSerializer::is_valid_json(""));
}

// ---------------------------------------------------------------------------
// Serialization must never throw across the worker boundary (finding #1)
// ---------------------------------------------------------------------------

TEST_CASE("Ill-formed UTF-8 yields a Status instead of throwing",
          "[serialization][utf8]") {
  JsonSerializer serializer;

  SECTION("in a property value") {
    Event e = make_event();
    e.properties = {{"note", std::string("\x80\x81\xFE")}};
    std::string out = "unchanged";
    const auto status = serializer.serialize(e, &out);
    CHECK_FALSE(status.ok());
    CHECK(status.code() == ErrorCode::Internal);
    CHECK(out.empty());
  }

  SECTION("in the player id") {
    Event e = make_event();
    e.player_id = std::string("\xC3");  // truncated two-byte sequence
    std::string out;
    CHECK_FALSE(serializer.serialize(e, &out).ok());
  }

  SECTION("in the event name") {
    Event e = make_event();
    e.name = std::string("bad\xED\xA0\x80");  // surrogate half
    std::string out;
    CHECK_FALSE(serializer.serialize(e, &out).ok());
  }
}

TEST_CASE("Embedded NUL round-trips through serialization",
          "[serialization][utf8]") {
  Event e = make_event();
  const std::string with_nul("a\0b", 3);
  e.properties = {{"note", with_nul}};

  const std::string text = serialize_ok(e);
  auto j = nlohmann::json::parse(text);
  CHECK(j.at("properties").at("note").get<std::string>() == with_nul);
  // The serialized form escapes it rather than emitting a raw NUL byte.
  CHECK(text.find('\0') == std::string::npos);
}
