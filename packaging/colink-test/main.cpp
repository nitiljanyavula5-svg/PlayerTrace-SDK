// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Uses an application-owned SQLite and PlayerTrace (which embeds its own,
// renamed SQLite) in the same process. Linking at all is most of the test; the
// runtime checks then confirm both copies are independently usable.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include <sqlite3.h>  // the APPLICATION's SQLite, with unprefixed symbols

#include <playertrace/playertrace.hpp>

int main() {
  // 1. The application's own SQLite works normally.
  std::printf("app sqlite version: %s\n", sqlite3_libversion());

  sqlite3* db = nullptr;
  if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
    std::fprintf(stderr, "app sqlite could not open a database\n");
    return 1;
  }
  if (sqlite3_exec(db, "CREATE TABLE t(a INTEGER); INSERT INTO t VALUES(1);",
                   nullptr, nullptr, nullptr) != SQLITE_OK) {
    std::fprintf(stderr, "app sqlite could not run a statement\n");
    sqlite3_close(db);
    return 1;
  }
  sqlite3_close(db);

  // 2. PlayerTrace works at the same time, using its own embedded copy.
  playertrace::Config config;
  config.app_id = "colink-test";
  config.storage_path = "./colink_test.db";
  config.consent = playertrace::ConsentState::Granted;

  auto created = playertrace::Client::create(config);
  if (!created.ok()) {
    std::fprintf(stderr, "playertrace create failed: %s\n",
                 created.status.message().c_str());
    return 1;
  }
  auto client = std::move(created.client);
  if (!client->start_session("anon").ok()) {
    std::fprintf(stderr, "start_session failed\n");
    return 1;
  }
  if (!client->track("colink_ok", {{"n", std::int64_t{1}}}).ok()) {
    std::fprintf(stderr, "track failed\n");
    return 1;
  }
  client->end_session();
  const auto flushed = client->flush(std::chrono::seconds(5));
  if (!flushed.ok()) {
    std::fprintf(stderr, "flush failed: %s\n", flushed.message().c_str());
    return 1;
  }
  client->shutdown(std::chrono::seconds(5));

  // 3. The application's SQLite is still healthy after PlayerTrace ran.
  std::printf("app sqlite version after playertrace: %s\n",
              sqlite3_libversion());
  std::printf("colink-test OK against PlayerTrace %s\n",
              playertrace::version());
  std::remove("./colink_test.db");
  std::remove("./colink_test.db-wal");
  std::remove("./colink_test.db-shm");
  std::remove("./colink-test-events.ndjson");
  return 0;
}
