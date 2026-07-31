// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Real crash-recovery testing (audit finding #19). A child process is killed
// with std::_Exit() — no destructors, no shutdown — and this process then
// verifies exactly what survived. The previous "survives a restart" test only
// performed an orderly flush and shutdown, which proves far less.
#include <catch2/catch_amalgamated.hpp>

#include <chrono>
#include <cstdlib>
#include <memory>
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

namespace {

Config base_config(const std::string& path,
                   std::shared_ptr<playertrace::Sink> sink) {
  Config c;
  c.app_id = "restart-test";
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

/// Runs the crash helper and returns its exit code. The helper path is provided
/// by CMake so the test does not have to guess the build layout.
int run_crash_helper(const std::string& db_path, const std::string& mode) {
#if defined(PLAYERTRACE_CRASH_HELPER)
  const std::string command = std::string("\"") + PLAYERTRACE_CRASH_HELPER +
                              "\" \"" + db_path + "\" " + mode;
#if defined(_WIN32)
  // cmd.exe strips one layer of quoting from the whole command line.
  const std::string wrapped = "\"" + command + "\"";
  return std::system(wrapped.c_str());
#else
  return std::system(command.c_str());
#endif
#else
  (void)db_path;
  (void)mode;
  return -1;
#endif
}

}  // namespace

#if defined(PLAYERTRACE_CRASH_HELPER)

TEST_CASE("Events persisted before a hard crash are recovered and delivered",
          "[restart][crash]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());

  const int code = run_crash_helper(db.path, "persist-then-crash");
  REQUIRE(code != -1);

  // The helper died without shutting down. A fresh client must find its work.
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(playertrace::ClientInternal::pending_in_store(*client) == 4);
  REQUIRE(client->flush(10s).ok());

  CHECK(names_of(*sink) ==
        std::vector<std::string>{"session_start", "alpha", "beta", "gamma"});
  CHECK(playertrace::ClientInternal::pending_in_store(*client) == 0);
}

TEST_CASE("Events only queued when a crash hits are lost, as documented",
          "[restart][crash]") {
  // This is the one documented loss window: track() returned Ok (Queued) but
  // the worker had not yet committed. The test pins the documented behavior so
  // a future change cannot quietly widen or narrow it unnoticed.
  pt_test::ScopedDb db(pt_test::unique_db_path());

  const int code = run_crash_helper(db.path, "accept-then-crash");
  REQUIRE(code != -1);

  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  REQUIRE(client->flush(10s).ok());

  // Whatever the worker managed to commit before the crash is delivered; the
  // rest is gone. Both are acceptable, but nothing may be duplicated or
  // corrupted, and the client must be perfectly usable.
  const auto delivered = names_of(*sink);
  CHECK(delivered.size() <= 4);
  for (const auto& name : delivered) {
    CHECK((name == "session_start" || name == "alpha" || name == "beta" ||
           name == "gamma"));
  }
  REQUIRE(client->start_session("anon").ok());
  REQUIRE(client->track("after_restart").ok());
  REQUIRE(client->flush(10s).ok());
}

TEST_CASE("A recovered database is not corrupted by the crash",
          "[restart][crash]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  REQUIRE(run_crash_helper(db.path, "persist-then-crash") != -1);

  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  // New work interleaves correctly with the recovered backlog.
  REQUIRE(client->start_session("anon").ok());
  REQUIRE(client->track("fresh").ok());
  REQUIRE(client->flush(10s).ok());

  const auto names = names_of(*sink);
  REQUIRE(names.size() == 6);
  // Recovered events come first (lower admission ordinals), then the new ones.
  CHECK(names[0] == "session_start");
  CHECK(names[1] == "alpha");
  CHECK(names[2] == "beta");
  CHECK(names[3] == "gamma");
  CHECK(names[4] == "session_start");
  CHECK(names[5] == "fresh");
}

#endif  // PLAYERTRACE_CRASH_HELPER

TEST_CASE("An orderly shutdown also preserves undelivered events",
          "[restart]") {
  // Deliberately named for what it proves: orderly shutdown, not a crash.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  {
    auto created = Client::create(
        base_config(db.path, std::make_shared<pt_test::FailingSink>()));
    REQUIRE(created.ok());
    auto client = std::move(created.client);
    client->start_session("anon");
    client->track("one");
    client->track("two");
    REQUIRE(pt_test::flush_persisted(*client, 5s));
    REQUIRE(client->shutdown(5s).ok());
  }
  {
    auto sink = std::make_shared<pt_test::CollectingSink>();
    auto created = Client::create(base_config(db.path, sink));
    REQUIRE(created.ok());
    auto client = std::move(created.client);
    REQUIRE(pt_test::flush_persisted(*client, 5s));
    CHECK(names_of(*sink) ==
          std::vector<std::string>{"session_start", "one", "two"});
  }
}
