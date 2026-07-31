// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_amalgamated.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "internal/event_validator.hpp"
#include "playertrace/event.hpp"

using playertrace::ErrorCode;
using playertrace::Properties;
using playertrace::PropertyValue;
using playertrace::internal::EventValidator;

namespace {
EventValidator make_validator() {
  return EventValidator(/*max_name=*/64, /*max_properties=*/8,
                        /*max_string=*/16, /*max_player_id=*/12);
}
}  // namespace

TEST_CASE("PropertyValue stores each supported type", "[event]") {
  PropertyValue b(true);
  PropertyValue i(std::int64_t{42});
  PropertyValue d(3.5);
  PropertyValue s(std::string("hello"));
  PropertyValue lit("world");
  PropertyValue plain_int(7);

  CHECK(b.type() == PropertyValue::Type::Bool);
  CHECK(b.as_bool() == true);
  CHECK(i.type() == PropertyValue::Type::Int);
  CHECK(i.as_int() == 42);
  CHECK(d.type() == PropertyValue::Type::Double);
  CHECK(d.as_double() == Catch::Approx(3.5));
  CHECK(s.type() == PropertyValue::Type::String);
  CHECK(s.as_string() == "hello");
  CHECK(lit.as_string() == "world");
  CHECK(plain_int.type() == PropertyValue::Type::Int);
  CHECK(plain_int.as_int() == 7);
}

TEST_CASE("Valid event names are accepted", "[event][validation]") {
  auto v = make_validator();
  CHECK(v.validate_name("level_started").ok());
  CHECK(v.validate_name("Player.Death-2").ok());
  CHECK(v.validate_name("_internal").ok());
}

TEST_CASE("Invalid event names are rejected", "[event][validation]") {
  auto v = make_validator();
  CHECK(v.validate_name("").code() == ErrorCode::InvalidEventName);
  CHECK(v.validate_name("2fast").code() == ErrorCode::InvalidEventName);
  CHECK(v.validate_name("has space").code() == ErrorCode::InvalidEventName);
  CHECK(v.validate_name("emoji\xF0\x9F\x98\x80").code() ==
        ErrorCode::InvalidEventName);
  CHECK(v.validate_name(std::string(65, 'a')).code() ==
        ErrorCode::InvalidEventName);
}

TEST_CASE("Reserved property keys are rejected", "[event][validation]") {
  auto v = make_validator();
  CHECK(v.validate_properties({{"event_id", true}}).code() ==
        ErrorCode::ReservedKey);
  CHECK(v.validate_properties({{"session_id", std::string("x")}}).code() ==
        ErrorCode::ReservedKey);
  CHECK(v.validate_properties({{"pt_internal", std::int64_t{1}}}).code() ==
        ErrorCode::ReservedKey);
  CHECK(EventValidator::is_reserved_key("seq"));
  CHECK_FALSE(EventValidator::is_reserved_key("level_id"));
}

TEST_CASE("Duplicate property keys are rejected", "[event][validation]") {
  auto v = make_validator();
  Properties props = {{"a", std::int64_t{1}}, {"a", std::int64_t{2}}};
  CHECK(v.validate_properties(props).code() == ErrorCode::DuplicateKey);
}

TEST_CASE("Too many properties are rejected", "[event][validation]") {
  auto v = make_validator();  // max 8
  Properties props;
  for (int i = 0; i < 9; ++i) {
    props.emplace_back("k" + std::to_string(i), std::int64_t{i});
  }
  CHECK(v.validate_properties(props).code() == ErrorCode::TooManyProperties);
}

TEST_CASE("Oversized string values are rejected", "[event][validation]") {
  auto v = make_validator();  // max string 16
  Properties ok = {{"note", std::string(16, 'x')}};
  Properties bad = {{"note", std::string(17, 'x')}};
  CHECK(v.validate_properties(ok).ok());
  CHECK(v.validate_properties(bad).code() == ErrorCode::ValueTooLarge);
}

TEST_CASE("Non-finite doubles are rejected", "[event][validation]") {
  auto v = make_validator();
  Properties nan = {{"x", std::numeric_limits<double>::quiet_NaN()}};
  Properties inf = {{"x", std::numeric_limits<double>::infinity()}};
  CHECK(v.validate_properties(nan).code() == ErrorCode::InvalidProperty);
  CHECK(v.validate_properties(inf).code() == ErrorCode::InvalidProperty);
}

TEST_CASE("Well-formed property maps validate", "[event][validation]") {
  auto v = make_validator();
  Properties props = {
      {"level_id", std::string("forest_01")},
      {"deaths", std::int64_t{3}},
      {"completion_seconds", 183.7},
      {"perfect", false},
  };
  CHECK(v.validate_properties(props).ok());
}

// ---------------------------------------------------------------------------
// UTF-8 validation (audit finding #1)
// ---------------------------------------------------------------------------

TEST_CASE("Well-formed UTF-8 is accepted", "[event][validation][utf8]") {
  CHECK(EventValidator::is_valid_utf8(""));
  CHECK(EventValidator::is_valid_utf8("plain ascii"));
  CHECK(EventValidator::is_valid_utf8("caf\xC3\xA9"));       // U+00E9
  CHECK(EventValidator::is_valid_utf8("\xE2\x82\xAC"));      // U+20AC
  CHECK(EventValidator::is_valid_utf8("\xF0\x9F\x98\x80"));  // U+1F600
  CHECK(EventValidator::is_valid_utf8("\xF4\x8F\xBF\xBF"));  // U+10FFFF
  // Embedded NUL is well-formed UTF-8 (U+0000) and must be accepted.
  CHECK(EventValidator::is_valid_utf8(std::string("a\0b", 3)));
}

TEST_CASE("Ill-formed UTF-8 is rejected", "[event][validation][utf8]") {
  SECTION("stray continuation byte") {
    CHECK_FALSE(EventValidator::is_valid_utf8("\x80"));
    CHECK_FALSE(EventValidator::is_valid_utf8("\x80\x81\xFE"));
  }
  SECTION("invalid lead byte") {
    CHECK_FALSE(EventValidator::is_valid_utf8("\xFE"));
    CHECK_FALSE(EventValidator::is_valid_utf8("\xFF"));
  }
  SECTION("truncated sequence") {
    CHECK_FALSE(EventValidator::is_valid_utf8("\xC3"));
    CHECK_FALSE(EventValidator::is_valid_utf8("\xE2\x82"));
    CHECK_FALSE(EventValidator::is_valid_utf8("\xF0\x9F\x98"));
  }
  SECTION("missing continuation byte") {
    CHECK_FALSE(EventValidator::is_valid_utf8("\xC3\x28"));
    CHECK_FALSE(EventValidator::is_valid_utf8("\xE2\x28\xA1"));
  }
  SECTION("overlong encoding") {
    CHECK_FALSE(EventValidator::is_valid_utf8("\xC0\xAF"));
    CHECK_FALSE(EventValidator::is_valid_utf8("\xE0\x80\xAF"));
    CHECK_FALSE(EventValidator::is_valid_utf8("\xF0\x80\x80\xAF"));
  }
  SECTION("UTF-16 surrogate half") {
    CHECK_FALSE(EventValidator::is_valid_utf8("\xED\xA0\x80"));  // U+D800
    CHECK_FALSE(EventValidator::is_valid_utf8("\xED\xBF\xBF"));  // U+DFFF
  }
  SECTION("beyond U+10FFFF") {
    CHECK_FALSE(EventValidator::is_valid_utf8("\xF4\x90\x80\x80"));
    CHECK_FALSE(EventValidator::is_valid_utf8("\xF5\x80\x80\x80"));
  }
}

TEST_CASE("Ill-formed UTF-8 property values are rejected",
          "[event][validation][utf8]") {
  auto v = make_validator();
  Properties bad = {{"note", std::string("\x80\x81")}};
  const auto status = v.validate_properties(bad);
  CHECK_FALSE(status.ok());
  CHECK(status.code() == ErrorCode::InvalidProperty);

  Properties truncated = {{"note", std::string("\xE2\x82")}};
  CHECK(v.validate_properties(truncated).code() == ErrorCode::InvalidProperty);

  Properties good = {{"note", std::string("caf\xC3\xA9")}};
  CHECK(v.validate_properties(good).ok());
}

TEST_CASE("Player ids are length- and UTF-8-validated",
          "[event][validation][utf8]") {
  auto v = make_validator();             // max player id 12
  CHECK(v.validate_player_id("").ok());  // no id supplied
  CHECK(v.validate_player_id("anon-42").ok());
  CHECK(v.validate_player_id(std::string(12, 'a')).ok());
  CHECK(v.validate_player_id(std::string(13, 'a')).code() ==
        ErrorCode::ValueTooLarge);
  CHECK(v.validate_player_id("\x80\x81").code() == ErrorCode::InvalidProperty);
  CHECK(v.validate_player_id("\xF0\x9F\x98").code() ==
        ErrorCode::InvalidProperty);  // truncated
}
