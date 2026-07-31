// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <thread>
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

/// Base config with the worker kept idle (long batch_interval) so tests drive
/// processing explicitly via flush() — no reliance on wall-clock timing.
Config base_config(const std::string& path,
                   std::shared_ptr<playertrace::Sink> sink) {
  Config c;
  c.app_id = "client-test";
  c.storage_path = path;
  c.consent = ConsentState::Granted;
  c.sink = std::move(sink);
  c.batch_interval = std::chrono::minutes(10);
  return c;
}

/// Names of delivered events, in the order the sink received them.
std::vector<std::string> delivered_names(const pt_test::CollectingSink& sink) {
  std::vector<std::string> names;
  for (const auto& e : sink.events()) {
    names.push_back(
        nlohmann::json::parse(e.json).at("name").get<std::string>());
  }
  return names;
}

/// Sequence numbers of delivered events, in delivery order.
std::vector<std::uint64_t> delivered_seqs(const pt_test::CollectingSink& sink) {
  std::vector<std::uint64_t> seqs;
  for (const auto& e : sink.events()) {
    seqs.push_back(
        nlohmann::json::parse(e.json).at("seq").get<std::uint64_t>());
  }
  return seqs;
}

}  // namespace

TEST_CASE("Consent defaults block collection until granted",
          "[client][consent]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = base_config(db.path, sink);
  config.consent = ConsentState::Unknown;  // explicit: no collection yet

  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  // A session is not even opened while consent is withheld: activating one
  // would risk emitting a session_end later with no matching session_start.
  CHECK(client->start_session("anon").code() == ErrorCode::ConsentDenied);
  CHECK(client->track("blocked").code() == ErrorCode::ConsentDenied);

  client->set_consent(ConsentState::Granted);
  REQUIRE(client->start_session("anon").ok());
  CHECK(client->track("allowed").ok());
  REQUIRE(client->flush(5s).ok());
  CHECK(delivered_names(*sink) ==
        std::vector<std::string>{"session_start", "allowed"});
}

TEST_CASE("track before a session returns NotStarted", "[client][session]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto created = Client::create(
      base_config(db.path, std::make_shared<pt_test::CollectingSink>()));
  REQUIRE(created.ok());
  CHECK(created.client->track("early").code() == ErrorCode::NotStarted);
}

TEST_CASE("Basic end-to-end delivery preserves order", "[client]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  CHECK(client->track("a").ok());
  CHECK(client->track("b").ok());
  REQUIRE(client->flush(5s).ok());
  CHECK(delivered_names(*sink) ==
        std::vector<std::string>{"session_start", "a", "b"});
  CHECK(delivered_seqs(*sink) == std::vector<std::uint64_t>{0, 1, 2});
}

TEST_CASE("flush and shutdown are idempotent", "[client][lifecycle]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto created = Client::create(
      base_config(db.path, std::make_shared<pt_test::CollectingSink>()));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session();
  client->track("x");
  CHECK(client->flush(5s).ok());
  CHECK(client->flush(5s).ok());  // second flush is a no-op Ok
  CHECK(client->shutdown(5s).ok());
  CHECK(client->shutdown(5s).ok());  // idempotent
  CHECK(client->track("after").code() == ErrorCode::AlreadyShutdown);
  CHECK(client->flush(5s).code() == ErrorCode::AlreadyShutdown);
}

// ---------------------------------------------------------------------------
// Durable capacity is enforced at admission (finding #13)
// ---------------------------------------------------------------------------

TEST_CASE("Storage cap rejects synchronously and keeps the oldest events",
          "[client][storage]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::FailingSink>();  // nothing ever acked
  Config config = base_config(db.path, sink);
  config.max_pending_events = 5;

  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("anon").ok());  // admitted event #1
  int accepted = 0;
  int rejected = 0;
  std::vector<std::string> accepted_ids;
  for (int i = 0; i < 20; ++i) {
    const auto s = client->track("evt", {{"i", std::int64_t{i}}});
    if (s.ok()) {
      ++accepted;
      accepted_ids.push_back("evt" + std::to_string(i));
    } else {
      // The caller is told synchronously, as documented.
      CHECK(s.code() == ErrorCode::StorageFull);
      ++rejected;
    }
  }
  REQUIRE(pt_test::flush_persisted(*client, 5s));

  CHECK(accepted == 4);  // + session_start == the cap of 5
  CHECK(rejected == 16);

  auto stats = playertrace::ClientInternal::stats(*client);
  CHECK(stats.accepted == 5);
  CHECK(stats.dropped_storage_full == 16);
  CHECK(stats.persisted == 5);
  CHECK(stats.delivered == 0);

  // Identity check: exactly the first four "evt" events survived, in order.
  CHECK(playertrace::ClientInternal::pending_in_store(*client) == 5);
  CHECK(accepted_ids ==
        std::vector<std::string>{"evt0", "evt1", "evt2", "evt3"});
}

TEST_CASE("Recovered events count against the durable cap",
          "[client][storage][restart]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  {
    Config config =
        base_config(db.path, std::make_shared<pt_test::FailingSink>());
    config.max_pending_events = 4;
    auto created = Client::create(config);
    REQUIRE(created.ok());
    auto client = std::move(created.client);
    client->start_session("anon");
    client->track("one");
    client->track("two");
    client->track("three");  // cap reached
    REQUIRE(pt_test::flush_persisted(*client, 5s));
    client->shutdown(5s);
  }
  {
    Config config =
        base_config(db.path, std::make_shared<pt_test::FailingSink>());
    config.max_pending_events = 4;
    auto created = Client::create(config);
    REQUIRE(created.ok());
    auto client = std::move(created.client);
    // The 4 recovered rows already fill the cap, so the new session_start
    // cannot be admitted — and start_session must SAY so rather than reporting
    // Ok and leaving a session whose marker was never collected.
    CHECK(client->start_session("anon").code() == ErrorCode::StorageFull);
    CHECK(client->track("nope").code() == ErrorCode::NotStarted);
  }
}

// ---------------------------------------------------------------------------
// Sequence numbers and ordering (finding #4)
// ---------------------------------------------------------------------------

TEST_CASE("A rejected event does not consume a sequence number",
          "[client][queue][ordering]") {
  // Determinism note: the worker wakes as soon as `batch_size` events are
  // queued, so simply filling the queue and hoping it stays full is a race.
  // Instead the worker is parked inside the sink first; while it is blocked it
  // cannot drain, so the queue reliably reaches capacity.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::BlockingSink>();
  Config config = base_config(db.path, sink);
  config.max_queue_size = 2;
  config.batch_size = 1;

  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("anon").ok());  // seq 0
  REQUIRE(sink->wait_until_entered(10s));       // worker is now pinned

  CHECK(client->track("a").ok());  // seq 1; queue: [a]
  CHECK(client->track("b").ok());  // seq 2; queue: [a, b] -> full
  CHECK(client->track("rejected").code() == ErrorCode::QueueFull);

  sink->release();
  REQUIRE(client->flush(10s).ok());

  // Gap-free: the rejected event never took a sequence number, so "b" keeps 2.
  auto names = std::vector<std::string>();
  auto seqs = std::vector<std::uint64_t>();
  for (const auto& e : sink->events()) {
    auto j = nlohmann::json::parse(e.json);
    names.push_back(j.at("name").get<std::string>());
    seqs.push_back(j.at("seq").get<std::uint64_t>());
  }
  CHECK(names == std::vector<std::string>{"session_start", "a", "b"});
  CHECK(seqs == std::vector<std::uint64_t>{0, 1, 2});
}

TEST_CASE("Concurrent producers deliver in gap-free sequence order",
          "[client][concurrency][ordering]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = base_config(db.path, sink);
  config.max_queue_size = 4096;

  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");  // seq 0 (session_start)

  constexpr int kThreads = 4;
  constexpr int kPerThread = 250;
  std::atomic<int> accepted{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&client, &accepted, t] {
      for (int i = 0; i < kPerThread; ++i) {
        if (client->track("concurrent", {{"thread", std::int64_t{t}}}).ok()) {
          accepted.fetch_add(1);
        }
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  REQUIRE(client->flush(30s).ok());

  const int expected = accepted.load() + 1;  // + session_start
  const auto seqs = delivered_seqs(*sink);
  REQUIRE(static_cast<int>(seqs.size()) == expected);

  // Assert the actual delivered ORDER, not just the set of values: sequence
  // numbers must arrive strictly increasing and contiguous from zero.
  for (std::size_t i = 0; i < seqs.size(); ++i) {
    REQUIRE(seqs[i] == static_cast<std::uint64_t>(i));
  }
}

TEST_CASE("Session markers bracket concurrently tracked events",
          "[client][concurrency][session]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = base_config(db.path, sink);
  config.max_queue_size = 4096;

  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  std::atomic<bool> go{false};
  std::atomic<int> accepted{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 3; ++t) {
    threads.emplace_back([&] {
      while (!go.load()) {
        std::this_thread::yield();
      }
      for (int i = 0; i < 100; ++i) {
        if (client->track("mid").ok()) {
          accepted.fetch_add(1);
        }
      }
    });
  }

  REQUIRE(client->start_session("anon").ok());
  go.store(true);
  for (auto& th : threads) {
    th.join();
  }
  REQUIRE(client->end_session().ok());
  REQUIRE(client->flush(30s).ok());

  const auto names = delivered_names(*sink);
  REQUIRE(names.size() >= 2);
  // session_start is first and session_end is last: no event can be delivered
  // outside the markers of the session it belongs to.
  CHECK(names.front() == "session_start");
  CHECK(names.back() == "session_end");
  for (std::size_t i = 1; i + 1 < names.size(); ++i) {
    CHECK(names[i] == "mid");
  }
}

TEST_CASE("A new session may start before the previous one drains",
          "[client][session]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("a").ok());
  client->track("first");
  REQUIRE(client->start_session("b").ok());  // auto-ends session a
  client->track("second");
  REQUIRE(client->flush(5s).ok());

  auto events = sink->events();
  REQUIRE(events.size() == 5);  // start,a-event,end,start,b-event
  std::set<std::string> session_ids;
  for (const auto& e : events) {
    session_ids.insert(
        nlohmann::json::parse(e.json).at("session_id").get<std::string>());
  }
  CHECK(session_ids.size() == 2);
  CHECK(delivered_names(*sink) ==
        std::vector<std::string>{"session_start", "first", "session_end",
                                 "session_start", "second"});
  // Each session numbers its own events from zero.
  CHECK(delivered_seqs(*sink) == std::vector<std::uint64_t>{0, 1, 2, 0, 1});
}

// ---------------------------------------------------------------------------
// Validation at the public boundary (findings #1 and #15)
// ---------------------------------------------------------------------------

TEST_CASE("Ill-formed UTF-8 is rejected by track()", "[client][utf8]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  client->start_session("anon");

  CHECK(client->track("evt", {{"k", std::string("\x80\x81")}}).code() ==
        ErrorCode::InvalidProperty);
  CHECK(client->track("evt", {{"k", std::string("\xE2\x82")}}).code() ==
        ErrorCode::InvalidProperty);
  CHECK(client->track("evt", {{"k", std::string("\xED\xA0\x80")}}).code() ==
        ErrorCode::InvalidProperty);
  CHECK(client->track("evt", {{"k", std::string("\xC0\xAF")}}).code() ==
        ErrorCode::InvalidProperty);

  REQUIRE(client->flush(5s).ok());
  CHECK(delivered_names(*sink) == std::vector<std::string>{"session_start"});
}

TEST_CASE("Ill-formed or oversized player ids are rejected",
          "[client][utf8][session]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  Config config =
      base_config(db.path, std::make_shared<pt_test::CollectingSink>());
  config.max_player_id_length = 8;
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  CHECK(client->start_session(std::string("\x80\x81")).code() ==
        ErrorCode::InvalidProperty);
  CHECK(client->start_session(std::string(9, 'a')).code() ==
        ErrorCode::ValueTooLarge);
  CHECK(client->start_session("ok-id").ok());
}

TEST_CASE("Properties are validated before the privacy filter runs",
          "[client][validation][privacy]") {
  // A filter that drops everything must not be able to hide invalid input.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  Config config =
      base_config(db.path, std::make_shared<pt_test::CollectingSink>());
  config.property_filter = [](const std::string&,
                              const playertrace::PropertyValue&) {
    return false;  // drop every property
  };
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  client->start_session("anon");

  CHECK(client->track("evt", {{"a", 1}, {"a", 2}}).code() ==
        ErrorCode::DuplicateKey);
  CHECK(client->track("evt", {{"event_id", 1}}).code() ==
        ErrorCode::ReservedKey);
  CHECK(client->track("evt", {{"k", std::string("\x80")}}).code() ==
        ErrorCode::InvalidProperty);
  CHECK(client->track("evt", {{"bad key", 1}}).code() ==
        ErrorCode::InvalidProperty);
}

TEST_CASE("Property filter drops selected keys", "[client][privacy]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = base_config(db.path, sink);
  config.property_filter = [](const std::string& key,
                              const playertrace::PropertyValue&) {
    return key.rfind("secret", 0) != 0;  // drop keys starting with "secret"
  };

  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session();
  client->track("login", {
                             {"secret_token", std::string("hunter2")},
                             {"level_id", std::string("forest_01")},
                         });
  REQUIRE(client->flush(5s).ok());

  bool checked = false;
  for (const auto& e : sink->events()) {
    auto j = nlohmann::json::parse(e.json);
    if (j.at("name") == "login") {
      CHECK_FALSE(j.at("properties").contains("secret_token"));
      CHECK(j.at("properties").contains("level_id"));
      checked = true;
    }
  }
  CHECK(checked);
}

TEST_CASE("A throwing property filter drops the property, not the process",
          "[client][privacy]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = base_config(db.path, sink);
  config.property_filter = [](const std::string& key,
                              const playertrace::PropertyValue&) -> bool {
    if (key == "boom") {
      throw std::runtime_error("filter exploded");
    }
    return true;
  };
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  client->start_session();
  CHECK(client->track("evt", {{"boom", 1}, {"keep", 2}}).ok());
  REQUIRE(client->flush(5s).ok());

  for (const auto& e : sink->events()) {
    auto j = nlohmann::json::parse(e.json);
    if (j.at("name") == "evt") {
      CHECK_FALSE(j.at("properties").contains("boom"));
      CHECK(j.at("properties").contains("keep"));
    }
  }
}

// ---------------------------------------------------------------------------
// Configuration validation (finding #15)
// ---------------------------------------------------------------------------

TEST_CASE("Invalid configuration is rejected", "[client][config]") {
  auto make = [](void (*mutate)(Config&)) {
    Config c;
    c.app_id = "cfg-test";
    c.storage_path = ":memory:";
    c.sink = std::make_shared<pt_test::CollectingSink>();
    mutate(c);
    return Client::create(c);
  };

  SECTION("missing app_id") {
    auto r = make([](Config& c) { c.app_id.clear(); });
    CHECK(r.status.code() == ErrorCode::InvalidConfig);
    CHECK(r.client == nullptr);
  }
  SECTION("missing storage_path") {
    auto r = make([](Config& c) { c.storage_path.clear(); });
    CHECK(r.status.code() == ErrorCode::InvalidConfig);
  }
  SECTION("zero batch_interval would spin the worker") {
    auto r = make(
        [](Config& c) { c.batch_interval = std::chrono::milliseconds(0); });
    CHECK(r.status.code() == ErrorCode::InvalidConfig);
  }
  SECTION("negative batch_interval") {
    auto r = make(
        [](Config& c) { c.batch_interval = std::chrono::milliseconds(-5); });
    CHECK(r.status.code() == ErrorCode::InvalidConfig);
  }
  SECTION("zero capacities") {
    CHECK(make([](Config& c) { c.max_queue_size = 0; }).status.code() ==
          ErrorCode::InvalidConfig);
    CHECK(make([](Config& c) { c.batch_size = 0; }).status.code() ==
          ErrorCode::InvalidConfig);
    CHECK(make([](Config& c) { c.max_pending_events = 0; }).status.code() ==
          ErrorCode::InvalidConfig);
    CHECK(make([](Config& c) { c.max_name_length = 0; }).status.code() ==
          ErrorCode::InvalidConfig);
    CHECK(make([](Config& c) { c.max_string_length = 0; }).status.code() ==
          ErrorCode::InvalidConfig);
    CHECK(make([](Config& c) { c.max_player_id_length = 0; }).status.code() ==
          ErrorCode::InvalidConfig);
  }
  SECTION("absurd capacities are refused rather than truncated") {
    auto r = make(
        [](Config& c) { c.max_pending_events = static_cast<std::size_t>(-1); });
    CHECK(r.status.code() == ErrorCode::InvalidConfig);
  }
  SECTION("batch_size larger than the queue") {
    auto r = make([](Config& c) {
      c.max_queue_size = 4;
      c.batch_size = 8;
    });
    CHECK(r.status.code() == ErrorCode::InvalidConfig);
  }
  SECTION("a valid configuration still succeeds") {
    auto r = make([](Config&) {});
    CHECK(r.status.ok());
    CHECK(r.client != nullptr);
  }
}

TEST_CASE("An unopenable database surfaces as a create error",
          "[client][config][storage]") {
  Config c;
  c.app_id = "cfg-test";
  // A path inside a directory that does not exist cannot be opened.
  c.storage_path = "pt_missing_dir_xyz/does/not/exist.db";
  c.sink = std::make_shared<pt_test::CollectingSink>();
  auto r = Client::create(c);
  CHECK_FALSE(r.status.ok());
  CHECK(r.status.code() == ErrorCode::StorageError);
  CHECK(r.client == nullptr);
}

TEST_CASE("A throwing sink never crashes the client", "[client][sink]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto created = Client::create(
      base_config(db.path, std::make_shared<pt_test::ThrowingSink>()));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session();
  client->track("boom");
  REQUIRE(pt_test::flush_persisted(*client, 5s));  // no crash; events retained
  auto stats = playertrace::ClientInternal::stats(*client);
  CHECK(stats.delivery_failures > 0);
  CHECK(stats.delivered == 0);
  CHECK(playertrace::ClientInternal::pending_in_store(*client) == 2);
}
