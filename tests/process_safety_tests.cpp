// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Direct regression for audit finding #1: ill-formed UTF-8 accepted by track()
// used to escape as an nlohmann type_error from the worker thread and abort the
// host process. Reaching the assertions in these tests IS the survival check —
// if the defect returns, the test binary dies instead of failing.
#include <catch2/catch_amalgamated.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "internal/client_internal.hpp"
#include "playertrace/playertrace.hpp"
#include "test_support.hpp"

using namespace std::chrono_literals;
using playertrace::Client;
using playertrace::Config;
using playertrace::ConsentState;
using playertrace::ErrorCode;

namespace {

Config base_config(const std::string& path,
                   std::shared_ptr<playertrace::Sink> sink) {
  Config c;
  c.app_id = "process-safety";
  c.storage_path = path;
  c.consent = ConsentState::Granted;
  c.sink = std::move(sink);
  c.batch_interval = std::chrono::minutes(10);
  return c;
}

/// The exact byte sequences that used to terminate the process.
std::vector<std::string> hostile_strings() {
  return {
      std::string("\x80\x81\xFE"),      // stray continuation + invalid lead
      std::string("\xC3"),              // truncated 2-byte sequence
      std::string("\xE2\x82"),          // truncated 3-byte sequence
      std::string("\xF0\x9F\x98"),      // truncated 4-byte sequence
      std::string("\xC0\xAF"),          // overlong '/'
      std::string("\xE0\x80\xAF"),      // overlong
      std::string("\xED\xA0\x80"),      // UTF-16 surrogate half
      std::string("\xF4\x90\x80\x80"),  // beyond U+10FFFF
      std::string("\xFF\xFF\xFF\xFF"),  // never valid
      // Invalid bytes embedded in the middle of otherwise valid text. The
      // escapes are split so the following letters are not absorbed into them.
      std::string("ok\x80"
                  "mid\xFE"
                  "bad"),
  };
}

}  // namespace

TEST_CASE("Hostile UTF-8 in properties is rejected and the process survives",
          "[process_safety][utf8]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  REQUIRE(client->start_session("anon").ok());

  for (const auto& bad : hostile_strings()) {
    const auto status = client->track("evt", {{"payload", bad}});
    CHECK_FALSE(status.ok());
    CHECK(status.code() == ErrorCode::InvalidProperty);
  }

  // The pipeline is still healthy and still delivers good events.
  REQUIRE(client->track("good").ok());
  REQUIRE(client->flush(10s).ok());
  REQUIRE(client->shutdown(5s).ok());
  CHECK(sink->count() == 2);  // session_start + good
}

TEST_CASE("Hostile UTF-8 in a player id is rejected and the process survives",
          "[process_safety][utf8]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  for (const auto& bad : hostile_strings()) {
    const auto status = client->start_session(bad);
    CHECK_FALSE(status.ok());
  }

  REQUIRE(client->start_session("anon").ok());
  REQUIRE(client->track("good").ok());
  REQUIRE(client->flush(10s).ok());
  REQUIRE(client->shutdown(5s).ok());
  CHECK(sink->count() == 2);
}

TEST_CASE("Embedded NUL bytes flow through the pipeline intact",
          "[process_safety][utf8]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  REQUIRE(client->start_session("anon").ok());

  const std::string with_nul("before\0after", 12);
  REQUIRE(client->track("evt", {{"payload", with_nul}}).ok());
  REQUIRE(client->flush(10s).ok());

  REQUIRE(sink->count() == 2);
  const auto events = sink->events();
  // The stored payload is not truncated at the NUL byte.
  CHECK(events[1].json.find("before") != std::string::npos);
  CHECK(events[1].json.find("after") != std::string::npos);
}

TEST_CASE("Concurrent hostile input never destabilizes the pipeline",
          "[process_safety][utf8][concurrency]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = base_config(db.path, sink);
  config.max_queue_size = 4096;
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  REQUIRE(client->start_session("anon").ok());

  const auto bad_strings = hostile_strings();
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 100; ++i) {
        if ((i % 2) == 0) {
          client->track(
              "evt", {{"payload", bad_strings[(i + t) % bad_strings.size()]}});
        } else {
          client->track("evt", {{"payload", std::string("fine")}});
        }
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  REQUIRE(client->flush(30s).ok());
  REQUIRE(client->shutdown(10s).ok());

  // Half of each thread's attempts were valid, plus session_start.
  CHECK(sink->count() == 4 * 50 + 1);
}

TEST_CASE("A throwing sink is contained by the worker barrier",
          "[process_safety]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto created = Client::create(
      base_config(db.path, std::make_shared<pt_test::ThrowingSink>()));
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  REQUIRE(client->start_session("anon").ok());
  REQUIRE(client->track("evt").ok());
  REQUIRE(pt_test::flush_persisted(*client, 10s));
  REQUIRE(client->shutdown(5s).ok());
  // Reaching here means nothing escaped the worker thread.
  CHECK(playertrace::ClientInternal::stats(*client).delivery_failures > 0);
}
