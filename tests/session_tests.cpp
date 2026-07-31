// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_amalgamated.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "internal/client_internal.hpp"
#include "internal/clock.hpp"
#include "internal/id_generator.hpp"
#include "internal/session_manager.hpp"
#include "playertrace/client.hpp"
#include "test_support.hpp"

using playertrace::Client;
using playertrace::Config;
using playertrace::ConsentState;
using playertrace::internal::IdGenerator;
using playertrace::internal::ManualClock;
using playertrace::internal::SessionManager;

TEST_CASE("SessionManager assigns gap-free sequences", "[session]") {
  IdGenerator ids;
  ManualClock clock(1000);
  SessionManager session(&ids, &clock);

  CHECK_FALSE(session.active());
  std::string sid, pid;
  std::uint64_t seq = 999;
  CHECK_FALSE(session.next(&sid, &pid, &seq));  // no active session

  const std::string s1 = session.begin("player-1");
  CHECK(session.active());
  for (std::uint64_t expected = 0; expected < 5; ++expected) {
    CHECK(session.next(&sid, &pid, &seq));
    CHECK(seq == expected);
    CHECK(sid == s1);
    CHECK(pid == "player-1");
  }
}

TEST_CASE("New session resets the sequence and changes id", "[session]") {
  IdGenerator ids;
  ManualClock clock(0);
  SessionManager session(&ids, &clock);

  const std::string s1 = session.begin("");
  std::string sid, pid;
  std::uint64_t seq = 0;
  session.next(&sid, &pid, &seq);
  session.next(&sid, &pid, &seq);
  CHECK(seq == 1);

  const std::string s2 = session.begin("");
  CHECK(s1 != s2);
  session.next(&sid, &pid, &seq);
  CHECK(seq == 0);  // reset
  CHECK(sid == s2);
}

TEST_CASE("Session duration is measured from the injected clock", "[session]") {
  IdGenerator ids;
  ManualClock clock(5000);
  SessionManager session(&ids, &clock);
  session.begin("");
  clock.set(8500);
  std::int64_t peek = 0;
  CHECK(session.peek_duration(&peek));
  CHECK(peek == 3500);
  std::int64_t ended = 0;
  CHECK(session.end(&ended));
  CHECK(ended == 3500);
  CHECK_FALSE(session.active());
  CHECK_FALSE(session.peek_duration(&peek));  // no longer active
}

TEST_CASE("Client emits standard session events", "[session][client]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();

  Config config;
  config.app_id = "session-test";
  config.storage_path = db.path;
  config.consent = ConsentState::Granted;
  config.sink = sink;
  config.batch_interval = std::chrono::minutes(10);  // worker idle until flush

  auto created = playertrace::ClientInternal::create(
      config, std::make_shared<ManualClock>(0));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  client->track("level_started", {{"level_id", std::string("forest_01")}});
  client->end_session();
  REQUIRE(client->flush(std::chrono::seconds(5)).ok());

  auto events = sink->events();
  REQUIRE(events.size() == 3);
  auto e0 = nlohmann::json::parse(events[0].json);
  auto e1 = nlohmann::json::parse(events[1].json);
  auto e2 = nlohmann::json::parse(events[2].json);
  CHECK(e0.at("name") == "session_start");
  CHECK(e1.at("name") == "level_started");
  CHECK(e2.at("name") == "session_end");
  CHECK(e2.at("properties").contains("session_seconds"));
  // Sequence numbers are gap-free within the session.
  CHECK(e0.at("seq") == 0);
  CHECK(e1.at("seq") == 1);
  CHECK(e2.at("seq") == 2);
  // All three share one session id.
  CHECK(e0.at("session_id") == e2.at("session_id"));
}

TEST_CASE("Starting a new session ends the previous one", "[session][client]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();

  Config config;
  config.app_id = "session-test";
  config.storage_path = db.path;
  config.consent = ConsentState::Granted;
  config.sink = sink;
  config.batch_interval = std::chrono::minutes(10);

  auto created = playertrace::ClientInternal::create(
      config, std::make_shared<ManualClock>(0));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("a");
  client->start_session("b");  // should auto-end session "a"
  REQUIRE(client->flush(std::chrono::seconds(5)).ok());

  auto events = sink->events();
  std::set<std::string> names;
  std::set<std::string> session_ids;
  for (const auto& e : events) {
    auto j = nlohmann::json::parse(e.json);
    names.insert(j.at("name").get<std::string>());
    session_ids.insert(j.at("session_id").get<std::string>());
  }
  CHECK(names.count("session_start") == 1);
  CHECK(names.count("session_end") == 1);
  CHECK(session_ids.size() == 2);  // two distinct sessions
}
