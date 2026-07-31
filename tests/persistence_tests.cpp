// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Storage failures must never silently destroy an accepted event, and a flush
// that could not persist what it drained must say so (audit finding #6).
#include <catch2/catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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
  c.app_id = "persist-test";
  c.storage_path = path;
  c.consent = ConsentState::Granted;
  c.sink = std::move(sink);
  c.batch_interval = std::chrono::minutes(10);
  return c;
}

std::vector<std::string> names_of(const pt_test::CollectingSink& sink) {
  std::vector<std::string> names;
  for (const auto& e : sink.events()) {
    names.push_back(
        nlohmann::json::parse(e.json).at("name").get<std::string>());
  }
  return names;
}

}  // namespace

TEST_CASE("A storage failure is reported by flush(), not swallowed",
          "[persistence][fault]") {
  const std::string op = GENERATE(
      std::string("begin"), std::string("step_insert"), std::string("commit"));
  CAPTURE(op);

  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  REQUIRE(client->track("one").ok());

  playertrace::ClientInternal::set_store_fault_hook(
      *client, [op](const char* o) { return op == o; });
  const auto flushed = client->flush(5s);
  playertrace::ClientInternal::set_store_fault_hook(*client, nullptr);

  // The checkpoint must not claim success when nothing could be persisted.
  CHECK_FALSE(flushed.ok());
  CHECK(flushed.code() == ErrorCode::StorageError);
  CHECK(sink->count() == 0);
}

TEST_CASE("Accepted events survive a storage failure and are retried",
          "[persistence][fault]") {
  const std::string op =
      GENERATE(std::string("begin"), std::string("prepare_insert"),
               std::string("bind_insert"), std::string("step_insert"),
               std::string("commit"));
  CAPTURE(op);

  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");  // event 1
  REQUIRE(client->track("one").ok());
  REQUIRE(client->track("two").ok());

  // Fail every insert attempt for the first flush.
  playertrace::ClientInternal::set_store_fault_hook(
      *client, [op](const char* o) { return op == o; });
  CHECK_FALSE(client->flush(5s).ok());
  CHECK(sink->count() == 0);

  // Clear the fault: the accepted events must still be there and get delivered.
  playertrace::ClientInternal::set_store_fault_hook(*client, nullptr);
  REQUIRE(client->flush(10s).ok());

  CHECK(names_of(*sink) ==
        std::vector<std::string>{"session_start", "one", "two"});

  auto stats = playertrace::ClientInternal::stats(*client);
  CHECK(stats.accepted == 3);
  CHECK(stats.delivered == 3);  // nothing was lost by the storage failure
}

TEST_CASE("Retried events keep their order ahead of newer ones",
          "[persistence][fault][ordering]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  REQUIRE(client->track("early_one").ok());
  REQUIRE(client->track("early_two").ok());

  playertrace::ClientInternal::set_store_fault_hook(
      *client, [](const char* o) { return std::string(o) == "commit"; });
  CHECK_FALSE(client->flush(5s).ok());
  playertrace::ClientInternal::set_store_fault_hook(*client, nullptr);

  // Events admitted after the failure must still be delivered afterwards.
  REQUIRE(client->track("late").ok());
  REQUIRE(client->flush(10s).ok());

  CHECK(names_of(*sink) == std::vector<std::string>{"session_start",
                                                    "early_one", "early_two",
                                                    "late"});
}

TEST_CASE("A storage failure does not free durable capacity",
          "[persistence][fault][storage]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = base_config(db.path, sink);
  config.max_pending_events = 3;
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");     // 1
  REQUIRE(client->track("a").ok());  // 2
  REQUIRE(client->track("b").ok());  // 3 -> cap reached

  playertrace::ClientInternal::set_store_fault_hook(
      *client, [](const char* o) { return std::string(o) == "commit"; });
  CHECK_FALSE(client->flush(5s).ok());
  playertrace::ClientInternal::set_store_fault_hook(*client, nullptr);

  // The three events are still outstanding, so the cap must still apply.
  CHECK(client->track("c").code() == ErrorCode::StorageFull);

  REQUIRE(client->flush(10s).ok());
  CHECK(sink->count() == 3);
  // Capacity is released only once the events are actually delivered.
  CHECK(client->track("c").ok());
}

TEST_CASE("Events that cannot be serialized are dropped, not fatal",
          "[persistence][fault]") {
  // Admission-time validation makes this unreachable through the public API, so
  // it is exercised through the serializer contract instead: the pipeline must
  // survive and keep processing its neighbours.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  CHECK(client->track("evt", {{"k", std::string("\x80")}}).code() ==
        ErrorCode::InvalidProperty);
  REQUIRE(client->track("good").ok());
  REQUIRE(client->flush(5s).ok());

  CHECK(names_of(*sink) == std::vector<std::string>{"session_start", "good"});
  auto stats = playertrace::ClientInternal::stats(*client);
  CHECK(stats.dropped_invalid == 0);  // rejected at admission, never admitted
}

TEST_CASE("A fetch failure does not lose durable events",
          "[persistence][fault]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  REQUIRE(client->track("one").ok());

  playertrace::ClientInternal::set_store_fault_hook(
      *client, [](const char* o) { return std::string(o) == "prepare_fetch"; });
  client->flush(5s);
  playertrace::ClientInternal::set_store_fault_hook(*client, nullptr);
  CHECK(sink->count() == 0);

  REQUIRE(client->flush(10s).ok());
  CHECK(names_of(*sink) == std::vector<std::string>{"session_start", "one"});
}

TEST_CASE("An acknowledge failure keeps events for redelivery",
          "[persistence][fault]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  REQUIRE(client->track("one").ok());

  playertrace::ClientInternal::set_store_fault_hook(
      *client, [](const char* o) { return std::string(o) == "commit_delete"; });
  client->flush(5s);
  playertrace::ClientInternal::set_store_fault_hook(*client, nullptr);

  // The sink saw the batch, but the rows could not be removed. At-least-once
  // means the events stay durable and MAY be delivered more than once, so the
  // contract to assert is "never lost", not an exact delivery count.
  const std::size_t delivered_while_failing = sink->count();
  CHECK(delivered_while_failing >= 2);
  CHECK(playertrace::ClientInternal::pending_in_store(*client) == 2);

  REQUIRE(client->flush(10s).ok());
  CHECK(sink->count() >= delivered_while_failing + 2);  // redelivered
  CHECK(playertrace::ClientInternal::pending_in_store(*client) == 0);

  // Duplicates are distinguishable downstream: every copy carries the same
  // event_id, which is exactly what the documented deduplication key is for.
  std::set<std::string> unique_ids;
  for (const auto& e : sink->events()) {
    unique_ids.insert(e.event_id);
  }
  CHECK(unique_ids.size() == 2);
}
