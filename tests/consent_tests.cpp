// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Consent-revocation semantics (audit finding #2). Revocation must be complete
// when set_consent() returns: nothing queued, nothing persisted, nothing that a
// quick re-grant or a restart can bring back.
#include <catch2/catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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
  c.app_id = "consent-test";
  c.storage_path = path;
  c.consent = ConsentState::Granted;
  c.sink = std::move(sink);
  c.batch_interval = std::chrono::minutes(10);
  return c;
}

}  // namespace

TEST_CASE("Revocation purges a persisted backlog", "[consent][storage]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  // A failing sink guarantees the events really are sitting in SQLite, which is
  // what the old test never established.
  auto sink = std::make_shared<pt_test::FailingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  client->track("one");
  client->track("two");
  REQUIRE(pt_test::flush_persisted(*client, 5s));
  REQUIRE(playertrace::ClientInternal::pending_in_store(*client) == 3);

  client->set_consent(ConsentState::Denied);

  // Complete on return: no waiting, no flush needed.
  CHECK(playertrace::ClientInternal::pending_in_store(*client) == 0);
  CHECK(client->track("after").code() == ErrorCode::ConsentDenied);
}

TEST_CASE("Revoked data does not survive a restart", "[consent][restart]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  {
    auto created = Client::create(
        base_config(db.path, std::make_shared<pt_test::FailingSink>()));
    REQUIRE(created.ok());
    auto client = std::move(created.client);
    client->start_session("anon");
    client->track("secret_one");
    client->track("secret_two");
    REQUIRE(pt_test::flush_persisted(*client, 5s));
    REQUIRE(playertrace::ClientInternal::pending_in_store(*client) == 3);

    client->set_consent(ConsentState::Denied);
    // No shutdown, no flush: the purge must already be committed to disk.
  }
  {
    auto sink = std::make_shared<pt_test::CollectingSink>();
    auto created = Client::create(base_config(db.path, sink));
    REQUIRE(created.ok());
    auto client = std::move(created.client);
    REQUIRE(pt_test::flush_persisted(*client, 5s));
    CHECK(sink->count() == 0);
    CHECK(playertrace::ClientInternal::pending_in_store(*client) == 0);
  }
}

TEST_CASE("An immediate re-grant does not revive revoked work",
          "[consent][race]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  client->track("stale_one");
  client->track("stale_two");

  client->set_consent(ConsentState::Denied);
  client->set_consent(ConsentState::Granted);  // immediate re-grant

  REQUIRE(client->start_session("anon").ok());
  REQUIRE(client->track("fresh").ok());
  REQUIRE(client->flush(5s).ok());

  // Only post-re-grant events are delivered.
  for (const auto& e : sink->events()) {
    CHECK(e.json.find("stale_one") == std::string::npos);
    CHECK(e.json.find("stale_two") == std::string::npos);
  }
  bool saw_fresh = false;
  for (const auto& e : sink->events()) {
    if (e.json.find("fresh") != std::string::npos) {
      saw_fresh = true;
    }
  }
  CHECK(saw_fresh);
}

TEST_CASE("An in-flight track cannot cross the revocation boundary",
          "[consent][race]") {
  // The property filter runs on the calling thread inside track(), before
  // admission. Blocking there gives a deterministic barrier: the call is
  // mid-flight exactly when consent is revoked and immediately re-granted.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = base_config(db.path, sink);

  std::mutex m;
  std::condition_variable entered_cv;
  std::condition_variable release_cv;
  bool entered = false;
  bool released = false;

  config.property_filter = [&](const std::string& key,
                               const playertrace::PropertyValue&) {
    if (key == "barrier") {
      {
        std::lock_guard<std::mutex> lock(m);
        entered = true;
      }
      entered_cv.notify_all();
      std::unique_lock<std::mutex> lock(m);
      release_cv.wait(lock, [&] { return released; });
    }
    return true;
  };

  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  REQUIRE(client->start_session("anon").ok());

  playertrace::Status track_status;
  std::thread producer(
      [&] { track_status = client->track("stale", {{"barrier", 1}}); });

  {
    std::unique_lock<std::mutex> lock(m);
    REQUIRE(entered_cv.wait_for(lock, 10s, [&] { return entered; }));
  }
  client->set_consent(ConsentState::Denied);
  client->set_consent(ConsentState::Granted);  // rapid re-grant
  {
    std::lock_guard<std::mutex> lock(m);
    released = true;
  }
  release_cv.notify_all();
  producer.join();

  CHECK_FALSE(track_status.ok());
  CHECK(track_status.code() == ErrorCode::ConsentDenied);

  REQUIRE(client->flush(5s).ok());
  for (const auto& e : sink->events()) {
    CHECK(e.json.find("stale") == std::string::npos);
  }
}

TEST_CASE("Starting Denied against an existing database purges it",
          "[consent][restart]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  {
    auto created = Client::create(
        base_config(db.path, std::make_shared<pt_test::FailingSink>()));
    REQUIRE(created.ok());
    auto client = std::move(created.client);
    client->start_session("anon");
    client->track("old");
    REQUIRE(pt_test::flush_persisted(*client, 5s));
    REQUIRE(playertrace::ClientInternal::pending_in_store(*client) == 2);
    client->shutdown(5s);
  }
  {
    // A new run that opens with consent withheld must not deliver the backlog.
    auto sink = std::make_shared<pt_test::CollectingSink>();
    Config config = base_config(db.path, sink);
    config.consent = ConsentState::Denied;
    auto created = Client::create(config);
    REQUIRE(created.ok());
    auto client = std::move(created.client);

    // Make the state explicit; the worker also purges when it observes that
    // consent is not granted.
    client->set_consent(ConsentState::Denied);
    CHECK(playertrace::ClientInternal::pending_in_store(*client) == 0);
    REQUIRE(pt_test::flush_persisted(*client, 5s));
    CHECK(sink->count() == 0);
  }
}

TEST_CASE("Revocation after shutdown still purges durable storage",
          "[consent][lifecycle]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto created = Client::create(
      base_config(db.path, std::make_shared<pt_test::FailingSink>()));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  client->track("one");
  REQUIRE(pt_test::flush_persisted(*client, 5s));
  REQUIRE(client->shutdown(5s).ok());
  REQUIRE(playertrace::ClientInternal::pending_in_store(*client) == 2);

  // The worker is gone, but revocation must not leave the data recoverable.
  client->set_consent(ConsentState::Denied);
  CHECK(playertrace::ClientInternal::pending_in_store(*client) == 0);
}

TEST_CASE("Revocation reports a failed purge instead of hiding it",
          "[consent][fault]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  std::atomic<int> errors{0};
  Config config =
      base_config(db.path, std::make_shared<pt_test::FailingSink>());
  config.error_callback = [&](const playertrace::Status& s) {
    if (s.code() == ErrorCode::StorageError) {
      errors.fetch_add(1);
    }
  };
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  client->track("one");
  REQUIRE(pt_test::flush_persisted(*client, 5s));

  playertrace::ClientInternal::set_store_fault_hook(
      *client, [](const char* op) { return std::string(op) == "purge"; });
  client->set_consent(ConsentState::Denied);
  playertrace::ClientInternal::set_store_fault_hook(*client, nullptr);

  CHECK(errors.load() > 0);  // the failure is surfaced, not swallowed
}

TEST_CASE("Consent state is observable through the public API", "[consent]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto created = Client::create(
      base_config(db.path, std::make_shared<pt_test::CollectingSink>()));
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  CHECK(client->consent() == ConsentState::Granted);
  client->set_consent(ConsentState::Denied);
  CHECK(client->consent() == ConsentState::Denied);
  client->set_consent(ConsentState::Unknown);
  CHECK(client->consent() == ConsentState::Unknown);
  CHECK(client->track("nope").code() == ErrorCode::ConsentDenied);
}
