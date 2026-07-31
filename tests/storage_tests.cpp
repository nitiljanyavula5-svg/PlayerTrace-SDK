// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "internal/sqlite_store.hpp"
#include "test_support.hpp"

using playertrace::ErrorCode;
using playertrace::internal::FetchResult;
using playertrace::internal::kStoreSchemaVersion;
using playertrace::internal::SqliteStore;
using playertrace::internal::StoredRecord;

namespace {

StoredRecord record(const std::string& id, std::uint64_t ordinal,
                    const std::string& payload) {
  StoredRecord r;
  r.event_id = id;
  r.session_id = "session-1";
  r.sequence = ordinal;
  r.ordinal = ordinal;
  r.created_ms = static_cast<std::int64_t>(1000 + ordinal);
  r.payload = payload;
  return r;
}

std::string obj(const std::string& id) {
  return "{\"event_id\":\"" + id + "\"}";
}

std::size_t count_or_fail(SqliteStore& store) {
  std::size_t n = 0;
  REQUIRE(store.count(&n).ok());
  return n;
}

/// Runs raw SQL against a database file, bypassing SqliteStore.
void raw_exec(const std::string& path, const std::string& sql) {
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  const int rc = sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr);
  sqlite3_close(raw);
  REQUIRE(rc == SQLITE_OK);
}

/// Reads the journal mode of a database file without going through the store.
std::string raw_journal_mode(const std::string& path) {
  std::string mode;
  sqlite3* raw = nullptr;
  if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) {
    sqlite3_close(raw);
    return mode;
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(raw, "PRAGMA journal_mode;", -1, &stmt, nullptr) ==
      SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const unsigned char* t = sqlite3_column_text(stmt, 0);
      if (t != nullptr) {
        mode = reinterpret_cast<const char*>(t);
      }
    }
    sqlite3_finalize(stmt);
  }
  sqlite3_close(raw);
  return mode;
}

/// Returns the set of table names in a database file.
std::vector<std::string> raw_tables(const std::string& path) {
  std::vector<std::string> names;
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(raw,
                         "SELECT name FROM sqlite_master WHERE type='table' "
                         "ORDER BY name;",
                         -1, &stmt, nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const unsigned char* t = sqlite3_column_text(stmt, 0);
      if (t != nullptr) {
        names.emplace_back(reinterpret_cast<const char*>(t));
      }
    }
    sqlite3_finalize(stmt);
  }
  sqlite3_close(raw);
  return names;
}

}  // namespace

TEST_CASE("Insert, count, fetch and acknowledge", "[storage]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  SqliteStore store(db.path);
  REQUIRE(store.open().ok());

  std::vector<StoredRecord> recs = {record("a", 0, obj("a")),
                                    record("b", 1, obj("b")),
                                    record("c", 2, obj("c"))};
  REQUIRE(store.insert(recs).ok());
  CHECK(count_or_fail(store) == 3);

  FetchResult fetched = store.fetch_pending(10);
  REQUIRE(fetched.status.ok());
  REQUIRE(fetched.events.size() == 3);
  CHECK(fetched.events[0].event_id == "a");
  CHECK(fetched.events[2].event_id == "c");

  std::size_t acked = 0;
  REQUIRE(store.acknowledge({"a", "b"}, &acked).ok());
  CHECK(acked == 2);
  CHECK(count_or_fail(store) == 1);
  FetchResult remaining = store.fetch_pending(10);
  REQUIRE(remaining.events.size() == 1);
  CHECK(remaining.events[0].event_id == "c");
}

TEST_CASE("Delivery order follows the admission ordinal, not the clock",
          "[storage][ordering]") {
  // A wall clock that steps backwards must not reorder delivery.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  SqliteStore store(db.path);
  REQUIRE(store.open().ok());

  StoredRecord first = record("first", 1, obj("first"));
  first.created_ms = 9'000;  // later wall-clock time
  StoredRecord second = record("second", 2, obj("second"));
  second.created_ms = 1'000;  // clock jumped backwards
  StoredRecord third = record("third", 3, obj("third"));
  third.created_ms = 1'000;  // identical timestamp

  REQUIRE(store.insert({first, second, third}).ok());
  FetchResult fetched = store.fetch_pending(10);
  REQUIRE(fetched.events.size() == 3);
  CHECK(fetched.events[0].event_id == "first");
  CHECK(fetched.events[1].event_id == "second");
  CHECK(fetched.events[2].event_id == "third");
}

TEST_CASE("Insert is idempotent per event_id", "[storage]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  SqliteStore store(db.path);
  REQUIRE(store.open().ok());

  REQUIRE(store.insert({record("dup", 0, obj("dup"))}).ok());
  REQUIRE(store.insert({record("dup", 0, obj("dup"))}).ok());
  CHECK(count_or_fail(store) == 1);
}

TEST_CASE("Payloads with embedded NUL bytes are stored whole", "[storage]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  SqliteStore store(db.path);
  REQUIRE(store.open().ok());

  // JSON escapes NUL, but the binding must still use an explicit length so a
  // payload is never silently truncated at the first NUL byte.
  const std::string payload = std::string("{\"a\":\"x\"}");
  REQUIRE(store.insert({record("n", 0, payload)}).ok());
  FetchResult fetched = store.fetch_pending(10);
  REQUIRE(fetched.events.size() == 1);
  CHECK(fetched.events[0].json == payload);
}

TEST_CASE("Malformed rows are quarantined, not fatal", "[storage]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  SqliteStore store(db.path);
  REQUIRE(store.open().ok());

  REQUIRE(store.insert({record("good", 0, obj("good"))}).ok());
  REQUIRE(store.insert({record("bad", 1, "{ this is not json")}).ok());
  CHECK(count_or_fail(store) == 2);

  FetchResult fetched = store.fetch_pending(10);
  CHECK_FALSE(fetched.status.ok());
  CHECK(fetched.status.message().find("quarantined") != std::string::npos);
  REQUIRE(fetched.events.size() == 1);
  CHECK(fetched.events[0].event_id == "good");
  CHECK(count_or_fail(store) == 1);  // bad row was removed
}

TEST_CASE("A failed quarantine is not reported as successful",
          "[storage][fault]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  SqliteStore store(db.path);
  REQUIRE(store.open().ok());
  REQUIRE(store.insert({record("bad", 0, "not json")}).ok());

  store.set_fault_hook(
      [](const char* op) { return std::string(op) == "begin_quarantine"; });
  FetchResult fetched = store.fetch_pending(10);
  store.set_fault_hook(nullptr);

  CHECK_FALSE(fetched.status.ok());
  CHECK(fetched.status.message().find("could not quarantine") !=
        std::string::npos);
  // Capacity must NOT be released for a quarantine that never committed.
  CHECK(fetched.quarantined == 0);
  CHECK(count_or_fail(store) == 1);  // the row is still there
}

TEST_CASE("A committed quarantine reports its count once", "[storage]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  SqliteStore store(db.path);
  REQUIRE(store.open().ok());
  REQUIRE(store.insert({record("good", 0, obj("good"))}).ok());
  REQUIRE(store.insert({record("bad", 1, "not json")}).ok());

  FetchResult fetched = store.fetch_pending(10);
  CHECK(fetched.quarantined == 1);  // so the caller can free that capacity
  CHECK(fetched.events.size() == 1);
  CHECK(count_or_fail(store) == 1);
}

TEST_CASE("purge_all clears pending events", "[storage]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  SqliteStore store(db.path);
  REQUIRE(store.open().ok());
  REQUIRE(
      store.insert({record("a", 0, obj("a")), record("b", 1, obj("b"))}).ok());
  std::size_t purged = 0;
  REQUIRE(store.purge_and_clear_revocation(&purged).ok());
  CHECK(count_or_fail(store) == 0);
}

TEST_CASE("Events survive reopening the database", "[storage][restart]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  {
    SqliteStore store(db.path);
    REQUIRE(store.open().ok());
    REQUIRE(store.insert({record("x", 0, obj("x")), record("y", 1, obj("y"))})
                .ok());
    store.close();  // reopened below without acknowledging
  }
  {
    SqliteStore reopened(db.path);
    REQUIRE(reopened.open().ok());
    CHECK(count_or_fail(reopened) == 2);
    FetchResult fetched = reopened.fetch_pending(10);
    REQUIRE(fetched.events.size() == 2);
  }
}

// ---------------------------------------------------------------------------
// Fault injection: an accepted event must never be silently lost (finding #6)
// ---------------------------------------------------------------------------

TEST_CASE("Insert failures roll back and leave the store usable",
          "[storage][fault]") {
  const std::string op =
      GENERATE(std::string("begin"), std::string("prepare_insert"),
               std::string("bind_insert"), std::string("step_insert"),
               std::string("commit"));
  CAPTURE(op);

  pt_test::ScopedDb db(pt_test::unique_db_path());
  SqliteStore store(db.path);
  REQUIRE(store.open().ok());
  REQUIRE(store.insert({record("pre", 0, obj("pre"))}).ok());

  store.set_fault_hook([&op](const char* o) { return op == o; });
  const auto failed =
      store.insert({record("a", 1, obj("a")), record("b", 2, obj("b"))});
  store.set_fault_hook(nullptr);

  CHECK_FALSE(failed.ok());
  CHECK(failed.code() == ErrorCode::StorageError);
  // Nothing from the failed batch was committed...
  CHECK(count_or_fail(store) == 1);
  // ...the pre-existing row is untouched...
  FetchResult fetched = store.fetch_pending(10);
  REQUIRE(fetched.events.size() == 1);
  CHECK(fetched.events[0].event_id == "pre");
  // ...and the connection still works (no transaction left open).
  REQUIRE(store.insert({record("after", 3, obj("after"))}).ok());
  CHECK(count_or_fail(store) == 2);
}

TEST_CASE("count() failure is reported, never as an empty store",
          "[storage][fault]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  SqliteStore store(db.path);
  REQUIRE(store.open().ok());
  REQUIRE(store.insert({record("a", 0, obj("a"))}).ok());

  store.set_fault_hook(
      [](const char* op) { return std::string(op) == "count"; });
  std::size_t n = 12345;
  const auto status = store.count(&n);
  store.set_fault_hook(nullptr);

  CHECK_FALSE(status.ok());
  CHECK(status.code() == ErrorCode::StorageError);
  CHECK(count_or_fail(store) == 1);  // recovers once the fault is cleared
}

TEST_CASE("Fetch and purge failures surface as StorageError",
          "[storage][fault]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  SqliteStore store(db.path);
  REQUIRE(store.open().ok());
  REQUIRE(store.insert({record("a", 0, obj("a"))}).ok());

  store.set_fault_hook(
      [](const char* op) { return std::string(op) == "prepare_fetch"; });
  CHECK_FALSE(store.fetch_pending(10).status.ok());
  store.set_fault_hook(nullptr);

  store.set_fault_hook(
      [](const char* op) { return std::string(op) == "purge"; });
  std::size_t purged = 0;
  CHECK_FALSE(store.purge_and_clear_revocation(&purged).ok());
  store.set_fault_hook(nullptr);

  CHECK(count_or_fail(store) == 1);  // the event is still safely stored
}

TEST_CASE("Acknowledge failures leave events durable", "[storage][fault]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  SqliteStore store(db.path);
  REQUIRE(store.open().ok());
  REQUIRE(store.insert({record("a", 0, obj("a"))}).ok());

  store.set_fault_hook(
      [](const char* op) { return std::string(op) == "commit_delete"; });
  std::size_t acked = 0;
  CHECK_FALSE(store.acknowledge({"a"}, &acked).ok());
  store.set_fault_hook(nullptr);

  CHECK(count_or_fail(store) == 1);  // retained for redelivery
}

// ---------------------------------------------------------------------------
// Schema safety (finding #8)
// ---------------------------------------------------------------------------

TEST_CASE("A newer schema version is refused without modifying the database",
          "[storage][schema]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  // A database written by a hypothetical future version: only schema_meta.
  raw_exec(db.path,
           "CREATE TABLE schema_meta (key TEXT PRIMARY KEY, value TEXT NOT "
           "NULL); INSERT INTO schema_meta VALUES('schema_version','999');");
  const auto before = raw_tables(db.path);

  SqliteStore store(db.path);
  const auto status = store.open();
  store.close();

  CHECK_FALSE(status.ok());
  CHECK(status.code() == ErrorCode::StorageError);
  // Refusal must not have created the events table or the index.
  const auto after = raw_tables(db.path);
  CHECK(before == after);
  CHECK(std::find(after.begin(), after.end(), "events") == after.end());
}

TEST_CASE("Malformed schema versions are refused", "[storage][schema]") {
  const std::string value =
      GENERATE(std::string("not-a-version"), std::string(""), std::string("1x"),
               std::string("-1"), std::string(" 2"),
               std::string("99999999999999999999"));
  CAPTURE(value);

  pt_test::ScopedDb db(pt_test::unique_db_path());
  raw_exec(db.path,
           "CREATE TABLE schema_meta (key TEXT PRIMARY KEY, value TEXT NOT "
           "NULL); INSERT INTO schema_meta VALUES('schema_version','" +
               value + "');");
  const auto before = raw_tables(db.path);

  SqliteStore store(db.path);
  const auto status = store.open();
  store.close();

  CHECK_FALSE(status.ok());
  CHECK(status.code() == ErrorCode::StorageError);
  CHECK(raw_tables(db.path) == before);  // untouched
}

TEST_CASE("An older schema version is refused (no migration exists)",
          "[storage][schema]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  raw_exec(db.path,
           "CREATE TABLE schema_meta (key TEXT PRIMARY KEY, value TEXT NOT "
           "NULL); INSERT INTO schema_meta VALUES('schema_version','2');");
  SqliteStore store(db.path);
  const auto status = store.open();
  CHECK_FALSE(status.ok());
  CHECK(status.code() == ErrorCode::StorageError);
}

TEST_CASE("A schema_meta table with no version row is refused",
          "[storage][schema]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  raw_exec(db.path,
           "CREATE TABLE schema_meta (key TEXT PRIMARY KEY, value TEXT NOT "
           "NULL);");
  SqliteStore store(db.path);
  CHECK_FALSE(store.open().ok());
}

TEST_CASE("An unrecognized database with an events table is refused",
          "[storage][schema]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  raw_exec(db.path, "CREATE TABLE events (whatever TEXT);");
  SqliteStore store(db.path);
  CHECK_FALSE(store.open().ok());
}

TEST_CASE("A fresh database is created at the current schema version",
          "[storage][schema]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  SqliteStore store(db.path);
  REQUIRE(store.open().ok());
  store.close();

  const auto tables = raw_tables(db.path);
  CHECK(std::find(tables.begin(), tables.end(), "events") != tables.end());
  CHECK(std::find(tables.begin(), tables.end(), "schema_meta") != tables.end());

  SqliteStore reopened(db.path);
  CHECK(reopened.open().ok());  // the version it wrote is the one it accepts
  CHECK(kStoreSchemaVersion == 3);
}

TEST_CASE("Journal mode is observed rather than assumed", "[storage]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  SqliteStore store(db.path);
  REQUIRE(store.open().ok());
  CHECK(store.journal_mode() == "wal");  // a real file should grant WAL

  SqliteStore mem(":memory:");
  REQUIRE(mem.open().ok());
  CHECK(mem.journal_mode() == "memory");  // never silently reported as WAL
}

// ---------------------------------------------------------------------------
// Unusable identifiers must not cause infinite redelivery (re-audit finding #7)
// ---------------------------------------------------------------------------

TEST_CASE("A NULL event_id is rejected by the schema", "[storage][schema]") {
  // A rowid table's TEXT PRIMARY KEY does NOT imply NOT NULL in SQLite. A NULL
  // key could never be acknowledged (DELETE ... WHERE event_id = NULL matches
  // nothing), so the same row was delivered forever. v3 forbids it outright.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  {
    SqliteStore store(db.path);
    REQUIRE(store.open().ok());
  }
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(db.path.c_str(), &raw) == SQLITE_OK);
  const int rc = sqlite3_exec(raw,
                              "INSERT INTO events(event_id, session_id, seq, "
                              "ordinal, created_ms, payload) "
                              "VALUES(NULL,'s',0,1,0,'{}');",
                              nullptr, nullptr, nullptr);
  sqlite3_close(raw);
  CHECK(rc != SQLITE_OK);  // NOT NULL constraint refuses it
}

TEST_CASE("An empty event_id is rejected by the schema", "[storage][schema]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  {
    SqliteStore store(db.path);
    REQUIRE(store.open().ok());
  }
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(db.path.c_str(), &raw) == SQLITE_OK);
  const int rc = sqlite3_exec(raw,
                              "INSERT INTO events(event_id, session_id, seq, "
                              "ordinal, created_ms, payload) "
                              "VALUES('','s',0,1,0,'{}');",
                              nullptr, nullptr, nullptr);
  sqlite3_close(raw);
  CHECK(rc != SQLITE_OK);  // CHECK(length(event_id) > 0) refuses it
}

TEST_CASE("Acknowledging a row that is not there is an error, not success",
          "[storage]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  SqliteStore store(db.path);
  REQUIRE(store.open().ok());
  REQUIRE(store.insert({record("present", 0, obj("present"))}).ok());

  std::size_t removed = 0;
  const auto status = store.acknowledge({"absent"}, &removed);
  CHECK_FALSE(status.ok());  // zero rows deleted must not read as acknowledged
  CHECK(removed == 0);
  CHECK(count_or_fail(store) == 1);
}

// ---------------------------------------------------------------------------
// Durable consent revocation marker (re-audit finding #1)
// ---------------------------------------------------------------------------

TEST_CASE("The revocation marker survives until the purge commits",
          "[storage][consent]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  {
    SqliteStore store(db.path);
    REQUIRE(store.open().ok());
    REQUIRE(store.insert({record("a", 0, obj("a"))}).ok());

    bool owed = false;
    REQUIRE(store.revocation_pending(&owed).ok());
    CHECK_FALSE(owed);

    REQUIRE(store.mark_revocation_pending().ok());
    REQUIRE(store.revocation_pending(&owed).ok());
    CHECK(owed);  // committed before any deletion is attempted
  }
  {
    // A crash between the two phases leaves the marker behind, so a later run
    // still knows a purge is owed.
    SqliteStore reopened(db.path);
    REQUIRE(reopened.open().ok());
    bool owed = false;
    REQUIRE(reopened.revocation_pending(&owed).ok());
    CHECK(owed);
    CHECK(count_or_fail(reopened) == 1);  // data still present

    std::size_t purged = 0;
    REQUIRE(reopened.purge_and_clear_revocation(&purged).ok());
    CHECK(purged == 1);
    REQUIRE(reopened.revocation_pending(&owed).ok());
    CHECK_FALSE(owed);  // cleared in the same transaction as the deletion
    CHECK(count_or_fail(reopened) == 0);
  }
}

TEST_CASE("A failed purge leaves the marker in place", "[storage][consent]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  SqliteStore store(db.path);
  REQUIRE(store.open().ok());
  REQUIRE(store.insert({record("a", 0, obj("a"))}).ok());
  REQUIRE(store.mark_revocation_pending().ok());

  store.set_fault_hook(
      [](const char* op) { return std::string(op) == "commit_purge"; });
  std::size_t purged = 0;
  CHECK_FALSE(store.purge_and_clear_revocation(&purged).ok());
  store.set_fault_hook(nullptr);

  bool owed = false;
  REQUIRE(store.revocation_pending(&owed).ok());
  CHECK(owed);                       // still owed
  CHECK(count_or_fail(store) == 1);  // and the data is still there
}

// ---------------------------------------------------------------------------
// Exact v3 shape validation (re-audit finding #8)
// ---------------------------------------------------------------------------

TEST_CASE("A database claiming v3 with the wrong columns is refused",
          "[storage][schema]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  raw_exec(db.path,
           "CREATE TABLE schema_meta (key TEXT PRIMARY KEY NOT NULL, value "
           "TEXT NOT NULL);"
           "INSERT INTO schema_meta VALUES('schema_version','3');"
           "CREATE TABLE events (ordinal INTEGER);"
           "CREATE INDEX idx_events_order ON events(ordinal);");
  SqliteStore store(db.path);
  const auto status = store.open();
  CHECK_FALSE(status.ok());  // used to open, then fail later during fetch
  CHECK(status.code() == ErrorCode::StorageError);
}

TEST_CASE("A v3 database missing its delivery index is refused",
          "[storage][schema]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  raw_exec(db.path,
           "CREATE TABLE schema_meta (key TEXT PRIMARY KEY NOT NULL, value "
           "TEXT NOT NULL);"
           "INSERT INTO schema_meta VALUES('schema_version','3');"
           "CREATE TABLE events ("
           "  event_id TEXT PRIMARY KEY NOT NULL CHECK(length(event_id) > 0),"
           "  session_id TEXT NOT NULL, seq INTEGER NOT NULL,"
           "  ordinal INTEGER NOT NULL, created_ms INTEGER NOT NULL,"
           "  payload TEXT NOT NULL);");
  SqliteStore store(db.path);
  CHECK_FALSE(store.open().ok());
}

TEST_CASE("Refusing a database leaves its bytes untouched",
          "[storage][schema]") {
  // Inspection is read-only and happens BEFORE any PRAGMA, so a refusal cannot
  // rewrite the journal mode of a database this build does not understand.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  raw_exec(db.path,
           "PRAGMA journal_mode=DELETE;"
           "CREATE TABLE schema_meta (key TEXT PRIMARY KEY NOT NULL, value "
           "TEXT NOT NULL);"
           "INSERT INTO schema_meta VALUES('schema_version','999');");

  const std::string before = pt_test::read_all(db.path);
  const std::string mode_before = raw_journal_mode(db.path);

  SqliteStore store(db.path);
  const auto status = store.open();
  CHECK_FALSE(status.ok());

  CHECK(pt_test::read_all(db.path) == before);  // byte-for-byte identical
  CHECK(raw_journal_mode(db.path) == mode_before);
  CHECK(mode_before == "delete");  // not silently converted to WAL
}

TEST_CASE("In-memory databases are usable for tests", "[storage]") {
  SqliteStore store(":memory:");
  REQUIRE(store.open().ok());
  REQUIRE(store.insert({record("m", 0, obj("m"))}).ok());
  CHECK(count_or_fail(store) == 1);
}
