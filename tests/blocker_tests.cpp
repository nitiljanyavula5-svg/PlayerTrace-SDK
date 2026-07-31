// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Deterministic coverage for the four blockers raised against the merged tree:
//
//  1  A revocation whose DURABLE MARKER cannot be written must still fail
//     closed: collection stays off, an immediate re-grant is refused until the
//     purge obligation is discharged, and revoked-but-persisted events are
//     never delivered. The failed-marker, failed-purge, immediate-re-grant and
//     restart cases are exercised separately.
//  2  shutdown(timeout) must spend its budget on BOTH draining and joining, so
//     a cooperative sink that returns shortly after request_cancel() yields a
//     truthful terminal result inside the budget rather than a bare Timeout.
//  3  Schema v3 is validated exactly — schema_meta's shape, the event_id
//     non-empty CHECK, and that idx_events_order really indexes ordinal — and a
//     refusal happens BEFORE any mutating PRAGMA, leaving the file untouched.
//  4  Each flush token reports ITS OWN outcome, even when a later pass with a
//     different outcome completes before the earlier waiter reads.
#include <catch2/catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sqlite3.h>

#include "internal/client_internal.hpp"
#include "playertrace/playertrace.hpp"
#include "test_support.hpp"

using namespace std::chrono_literals;
using playertrace::Client;
using playertrace::Config;
using playertrace::ConsentState;
using playertrace::ErrorCode;
using playertrace::Status;

namespace {

Config blocker_config(const std::string& path,
                      std::shared_ptr<playertrace::Sink> sink) {
  Config c;
  c.app_id = "blockers";
  c.storage_path = path;
  c.consent = ConsentState::Granted;
  c.sink = std::move(sink);
  c.batch_interval = std::chrono::minutes(10);
  c.batch_size = 100;
  c.max_queue_size = 1000;
  return c;
}

/// Refuses every write, so events reach durable storage and stay there. That is
/// the state a revocation has to be able to purge.
class RefusingSink : public playertrace::Sink {
 public:
  Status write(const playertrace::EventBatch&) override {
    calls_.fetch_add(1);
    return Status(ErrorCode::SinkError, "blocker: sink refuses");
  }
  Status flush() override { return Status(); }
  int calls() const { return calls_.load(); }

 private:
  std::atomic<int> calls_{0};
};

}  // namespace

// ---------------------------------------------------------------------------
// Blocker 1 — failed revocation-marker admission
// ---------------------------------------------------------------------------

TEST_CASE("A revocation whose durable marker fails still fails closed",
          "[consent][blocker][revocation]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<RefusingSink>();
  auto created = Client::create(blocker_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  REQUIRE(client->track("secret").ok());
  // The sink refuses, so these are persisted-but-undelivered: exactly the rows
  // a revocation is obliged to destroy.
  client->flush(5s);
  REQUIRE(playertrace::ClientInternal::pending_in_store(*client) == 2);

  playertrace::ClientInternal::set_store_fault_hook(
      *client,
      [](const char* op) { return std::string(op) == "mark_revocation"; });

  const Status denied = client->set_consent(ConsentState::Denied);
  CHECK_FALSE(denied.ok());
  CHECK(denied.code() == ErrorCode::StorageError);

  // Collection is off even though the marker could not be written.
  CHECK(client->consent() != ConsentState::Granted);
  CHECK(client->track("leak").code() == ErrorCode::ConsentDenied);
  CHECK(client->start_session("p2").code() == ErrorCode::ConsentDenied);

  // The revoked rows must never reach the sink, whatever happens next.
  const int calls_before = sink->calls();
  client->flush(2s);
  CHECK(sink->calls() == calls_before);

  playertrace::ClientInternal::set_store_fault_hook(*client, nullptr);
  client->shutdown(5s);
}

TEST_CASE("A revocation whose purge fails keeps the obligation and the data",
          "[consent][blocker][revocation]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<RefusingSink>();
  auto created = Client::create(blocker_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  REQUIRE(client->track("secret").ok());
  client->flush(5s);
  REQUIRE(playertrace::ClientInternal::pending_in_store(*client) == 2);

  playertrace::ClientInternal::set_store_fault_hook(
      *client, [](const char* op) { return std::string(op) == "purge"; });

  const Status denied = client->set_consent(ConsentState::Denied);
  CHECK_FALSE(denied.ok());
  CHECK(denied.code() == ErrorCode::StorageError);
  CHECK(client->consent() != ConsentState::Granted);

  // Fail closed: the rows are still there (the purge did NOT happen), and the
  // accounting was deliberately not reset to pretend otherwise.
  CHECK(playertrace::ClientInternal::pending_in_store(*client) == 2);
  CHECK(client->track("leak").code() == ErrorCode::ConsentDenied);

  // Once the fault clears, the owed purge completes and the data is gone.
  playertrace::ClientInternal::set_store_fault_hook(*client, nullptr);
  CHECK(client->set_consent(ConsentState::Granted).ok());
  CHECK(playertrace::ClientInternal::pending_in_store(*client) == 0);
  client->shutdown(5s);
}

TEST_CASE("An immediate re-grant is refused while a purge is still owed",
          "[consent][blocker][revocation]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<RefusingSink>();
  auto created = Client::create(blocker_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  REQUIRE(client->track("secret").ok());
  client->flush(5s);

  playertrace::ClientInternal::set_store_fault_hook(
      *client,
      [](const char* op) { return std::string(op) == "mark_revocation"; });
  CHECK_FALSE(client->set_consent(ConsentState::Denied).ok());

  // Re-granting must NOT quietly re-enable collection: the obligation is still
  // outstanding, so the attempt is refused and consent stays withheld.
  for (int attempt = 0; attempt < 3; ++attempt) {
    const Status regrant = client->set_consent(ConsentState::Granted);
    INFO("re-grant attempt " << attempt);
    CHECK_FALSE(regrant.ok());
    CHECK(regrant.code() == ErrorCode::StorageError);
    CHECK(client->consent() != ConsentState::Granted);
    CHECK(client->track("leak").code() == ErrorCode::ConsentDenied);
  }

  // Only once the store cooperates does the re-grant succeed.
  playertrace::ClientInternal::set_store_fault_hook(*client, nullptr);
  CHECK(client->set_consent(ConsentState::Granted).ok());
  CHECK(client->consent() == ConsentState::Granted);
  CHECK(playertrace::ClientInternal::pending_in_store(*client) == 0);
  client->shutdown(5s);
}

TEST_CASE("A revocation owed at restart is discharged before anything is sent",
          "[consent][blocker][revocation][restart]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());

  // Run 1: persist events, then revoke with the PURGE failing. The marker is
  // committed, so the obligation survives the process boundary.
  {
    auto sink = std::make_shared<RefusingSink>();
    auto created = Client::create(blocker_config(db.path, sink));
    REQUIRE(created.ok());
    auto client = std::move(created.client);
    REQUIRE(client->start_session("p1").ok());
    REQUIRE(client->track("secret").ok());
    client->flush(5s);
    REQUIRE(playertrace::ClientInternal::pending_in_store(*client) == 2);

    playertrace::ClientInternal::set_store_fault_hook(
        *client, [](const char* op) { return std::string(op) == "purge"; });
    CHECK_FALSE(client->set_consent(ConsentState::Denied).ok());
    CHECK(playertrace::ClientInternal::pending_in_store(*client) == 2);
    playertrace::ClientInternal::set_store_fault_hook(*client, nullptr);
    client->shutdown(5s);
  }

  // Run 2: reopening must resolve the owed purge BEFORE any delivery. A sink
  // that records everything proves nothing revoked was ever handed over.
  {
    auto sink = std::make_shared<pt_test::CollectingSink>();
    auto created = Client::create(blocker_config(db.path, sink));
    REQUIRE(created.ok());
    auto client = std::move(created.client);
    CHECK(playertrace::ClientInternal::pending_in_store(*client) == 0);
    REQUIRE(client->start_session("p2").ok());
    REQUIRE(client->flush(10s).ok());
    // Only the new session's own marker; nothing recovered from run 1.
    for (const auto& e : sink->events()) {
      INFO("delivered " << e.json);
      CHECK(e.json.find("secret") == std::string::npos);
    }
    CHECK(sink->count() == 1);
    client->shutdown(5s);
  }
}

// ---------------------------------------------------------------------------
// Blocker 2 — shutdown budget
// ---------------------------------------------------------------------------

namespace {

/// Blocks inside write() until request_cancel() arrives, then returns promptly.
/// This is the cooperative contract a well-behaved sink implements, and
/// shutdown must be able to complete within its budget when it is honoured.
class CooperativeSink : public playertrace::Sink {
 public:
  Status write(const playertrace::EventBatch&) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      entered_ = true;
    }
    entered_cv_.notify_all();
    std::unique_lock<std::mutex> lock(mutex_);
    released_cv_.wait(lock, [this] { return cancelled_; });
    return Status(ErrorCode::SinkError, "cooperative: cancelled");
  }
  Status flush() override { return Status(); }

  void request_cancel() noexcept override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancelled_ = true;
    }
    released_cv_.notify_all();
  }

  bool wait_until_entered(std::chrono::milliseconds t) {
    std::unique_lock<std::mutex> lock(mutex_);
    return entered_cv_.wait_for(lock, t, [this] { return entered_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable entered_cv_;
  std::condition_variable released_cv_;
  bool entered_ = false;
  bool cancelled_ = false;
};

}  // namespace

TEST_CASE("shutdown reaches a terminal state within its budget",
          "[lifecycle][blocker][shutdown]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<CooperativeSink>();
  Config config = blocker_config(db.path, sink);
  config.batch_interval = 10ms;  // let the worker reach the sink on its own
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  REQUIRE(client->track("wedged").ok());
  REQUIRE(sink->wait_until_entered(10s));  // worker is parked inside write()

  // The drain cannot finish while the sink is parked, so shutdown must spend
  // only part of its budget there and keep enough to cancel and join. Before
  // the budget was split, this returned Timeout with the worker still running.
  const auto start = std::chrono::steady_clock::now();
  const Status stopped = client->shutdown(4s);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  INFO("shutdown returned " << static_cast<int>(stopped.code()) << " after "
                            << elapsed.count() << "ms: " << stopped.message());
  CHECK(elapsed < 4s);
  CHECK(stopped.code() != ErrorCode::Timeout);
  // The worker genuinely exited and was joined; it was never abandoned.
  CHECK(playertrace::ClientInternal::worker_finished(*client));
  // A sink that refused the final batch is not a shutdown failure: the events
  // stay durable for the next run.
  CHECK(stopped.ok());
}

TEST_CASE("shutdown is idempotent and stays truthful when called again",
          "[lifecycle][blocker][shutdown]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(blocker_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  CHECK(client->shutdown(5s).ok());
  CHECK(playertrace::ClientInternal::worker_finished(*client));
  CHECK(client->shutdown(5s).ok());
  CHECK(client->track("after").code() == ErrorCode::AlreadyShutdown);
}

// ---------------------------------------------------------------------------
// Blocker 3 — exact schema-v3 validation
// ---------------------------------------------------------------------------

namespace {

/// Runs raw SQL against a database file, outside PlayerTrace, to plant a
/// deliberately wrong schema.
void raw_sql(const std::string& path, const std::vector<std::string>& stmts) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
  for (const auto& sql : stmts) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    INFO("sql: " << sql << " err: " << (err ? err : "(none)"));
    REQUIRE(rc == SQLITE_OK);
    sqlite3_free(err);
  }
  REQUIRE(sqlite3_close(db) == SQLITE_OK);
}

Status open_expecting_failure(const std::string& path) {
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(blocker_config(path, sink));
  return created.status;
}

}  // namespace

TEST_CASE("A database claiming v3 with the wrong schema_meta shape is refused",
          "[storage][blocker][schema]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  raw_sql(db.path,
          {"CREATE TABLE schema_meta (key TEXT PRIMARY KEY NOT NULL, value "
           "TEXT NOT NULL, extra TEXT);",
           "INSERT INTO schema_meta(key,value) VALUES('schema_version','3');",
           "CREATE TABLE events (event_id TEXT PRIMARY KEY NOT NULL "
           "CHECK(length(event_id) > 0), session_id TEXT NOT NULL, seq INTEGER "
           "NOT NULL, ordinal INTEGER NOT NULL, created_ms INTEGER NOT NULL, "
           "payload TEXT NOT NULL);",
           "CREATE INDEX idx_events_order ON events(ordinal);"});

  const Status status = open_expecting_failure(db.path);
  INFO(status.message());
  CHECK_FALSE(status.ok());
  CHECK(status.code() == ErrorCode::StorageError);
  CHECK(status.message().find("schema_meta") != std::string::npos);
}

TEST_CASE("A database missing the event_id non-empty CHECK is refused",
          "[storage][blocker][schema]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  // Column list is identical; only the CHECK constraint is gone. PRAGMA
  // table_info cannot see the difference, which is why the stored SQL is read.
  raw_sql(db.path,
          {"CREATE TABLE schema_meta (key TEXT PRIMARY KEY NOT NULL, value "
           "TEXT NOT NULL);",
           "INSERT INTO schema_meta(key,value) VALUES('schema_version','3');",
           "CREATE TABLE events (event_id TEXT PRIMARY KEY NOT NULL, "
           "session_id TEXT NOT NULL, seq INTEGER NOT NULL, ordinal INTEGER "
           "NOT NULL, created_ms INTEGER NOT NULL, payload TEXT NOT NULL);",
           "CREATE INDEX idx_events_order ON events(ordinal);"});

  const Status status = open_expecting_failure(db.path);
  INFO(status.message());
  CHECK_FALSE(status.ok());
  CHECK(status.code() == ErrorCode::StorageError);
  CHECK(status.message().find("CHECK") != std::string::npos);
}

TEST_CASE("An idx_events_order over the wrong column is refused",
          "[storage][blocker][schema]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  // The index EXISTS under the right name but indexes created_ms, so delivery
  // order would silently stop following the admission ordinal.
  raw_sql(db.path,
          {"CREATE TABLE schema_meta (key TEXT PRIMARY KEY NOT NULL, value "
           "TEXT NOT NULL);",
           "INSERT INTO schema_meta(key,value) VALUES('schema_version','3');",
           "CREATE TABLE events (event_id TEXT PRIMARY KEY NOT NULL "
           "CHECK(length(event_id) > 0), session_id TEXT NOT NULL, seq INTEGER "
           "NOT NULL, ordinal INTEGER NOT NULL, created_ms INTEGER NOT NULL, "
           "payload TEXT NOT NULL);",
           "CREATE INDEX idx_events_order ON events(created_ms);"});

  const Status status = open_expecting_failure(db.path);
  INFO(status.message());
  CHECK_FALSE(status.ok());
  CHECK(status.code() == ErrorCode::StorageError);
  CHECK(status.message().find("idx_events_order") != std::string::npos);
}

TEST_CASE("A CHECK constraint forged inside a string literal is refused",
          "[storage][blocker][schema]") {
  // The exact constraint text placed inside a DEFAULT string literal. Substring
  // matching on the stored DDL accepted this while no real constraint existed,
  // so empty identifiers could be written and never acknowledged.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  raw_sql(db.path,
          {"CREATE TABLE schema_meta (key TEXT PRIMARY KEY NOT NULL, value "
           "TEXT NOT NULL);",
           "INSERT INTO schema_meta(key,value) VALUES('schema_version','3');",
           "CREATE TABLE events (event_id TEXT PRIMARY KEY NOT NULL, "
           "session_id TEXT NOT NULL DEFAULT 'CHECK(length(event_id) > 0)', "
           "seq INTEGER NOT NULL, ordinal INTEGER NOT NULL, created_ms INTEGER "
           "NOT NULL, payload TEXT NOT NULL);",
           "CREATE INDEX idx_events_order ON events(ordinal);"});

  const Status status = open_expecting_failure(db.path);
  INFO(status.message());
  CHECK_FALSE(status.ok());
  CHECK(status.code() == ErrorCode::StorageError);
  CHECK(status.message().find("CHECK") != std::string::npos);
}

TEST_CASE("A CHECK constraint forged inside a comment is refused",
          "[storage][blocker][schema]") {
  // Same forgery, hidden in a comment instead of a literal.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  raw_sql(db.path,
          {"CREATE TABLE schema_meta (key TEXT PRIMARY KEY NOT NULL, value "
           "TEXT NOT NULL);",
           "INSERT INTO schema_meta(key,value) VALUES('schema_version','3');",
           "CREATE TABLE events (event_id TEXT PRIMARY KEY NOT NULL, /* "
           "CHECK(length(event_id) > 0) */ session_id TEXT NOT NULL, seq "
           "INTEGER NOT NULL, -- CHECK(length(event_id) > 0)\n ordinal INTEGER "
           "NOT NULL, created_ms INTEGER NOT NULL, payload TEXT NOT NULL);",
           "CREATE INDEX idx_events_order ON events(ordinal);"});

  const Status status = open_expecting_failure(db.path);
  INFO(status.message());
  CHECK_FALSE(status.ok());
  CHECK(status.code() == ErrorCode::StorageError);
  CHECK(status.message().find("CHECK") != std::string::npos);
}

TEST_CASE("A CHECK constraint with the wrong expression is refused",
          "[storage][blocker][schema]") {
  // A real constraint, but the wrong one: length > 1 would reject a
  // single-character identifier that the schema is required to accept, and
  // still permits nothing the non-empty rule was written to catch.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  raw_sql(db.path,
          {"CREATE TABLE schema_meta (key TEXT PRIMARY KEY NOT NULL, value "
           "TEXT NOT NULL);",
           "INSERT INTO schema_meta(key,value) VALUES('schema_version','3');",
           "CREATE TABLE events (event_id TEXT PRIMARY KEY NOT NULL "
           "CHECK(length(event_id) > 1), session_id TEXT NOT NULL, seq INTEGER "
           "NOT NULL, ordinal INTEGER NOT NULL, created_ms INTEGER NOT NULL, "
           "payload TEXT NOT NULL);",
           "CREATE INDEX idx_events_order ON events(ordinal);"});

  const Status status = open_expecting_failure(db.path);
  INFO(status.message());
  CHECK_FALSE(status.ok());
  CHECK(status.code() == ErrorCode::StorageError);
  CHECK(status.message().find("CHECK") != std::string::npos);
}

TEST_CASE("A partial idx_events_order is refused",
          "[storage][blocker][schema]") {
  // The index exists, is named correctly and indexes the right column — but a
  // WHERE clause excludes rows from it, so delivery order would silently stop
  // covering everything. PRAGMA index_list reports this structurally.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  raw_sql(db.path,
          {"CREATE TABLE schema_meta (key TEXT PRIMARY KEY NOT NULL, value "
           "TEXT NOT NULL);",
           "INSERT INTO schema_meta(key,value) VALUES('schema_version','3');",
           "CREATE TABLE events (event_id TEXT PRIMARY KEY NOT NULL "
           "CHECK(length(event_id) > 0), session_id TEXT NOT NULL, seq INTEGER "
           "NOT NULL, ordinal INTEGER NOT NULL, created_ms INTEGER NOT NULL, "
           "payload TEXT NOT NULL);",
           "CREATE INDEX idx_events_order ON events(ordinal) WHERE ordinal > "
           "0;"});

  const Status status = open_expecting_failure(db.path);
  INFO(status.message());
  CHECK_FALSE(status.ok());
  CHECK(status.code() == ErrorCode::StorageError);
  CHECK(status.message().find("idx_events_order") != std::string::npos);
}

TEST_CASE("Refusing a malformed schema leaves the file byte-for-byte unchanged",
          "[storage][blocker][schema]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  raw_sql(db.path,
          {"CREATE TABLE schema_meta (key TEXT PRIMARY KEY NOT NULL, value "
           "TEXT NOT NULL);",
           "INSERT INTO schema_meta(key,value) VALUES('schema_version','3');",
           "CREATE TABLE events (event_id TEXT PRIMARY KEY NOT NULL, "
           "session_id TEXT NOT NULL, seq INTEGER NOT NULL, ordinal INTEGER "
           "NOT NULL, created_ms INTEGER NOT NULL, payload TEXT NOT NULL);",
           "CREATE INDEX idx_events_order ON events(ordinal);"});

  // Capture the exact bytes, including any journal sidecars.
  const std::string before = pt_test::read_all(db.path);
  REQUIRE_FALSE(before.empty());

  const Status status = open_expecting_failure(db.path);
  REQUIRE_FALSE(status.ok());

  // The schema is inspected read-only and refused BEFORE journal_mode is set,
  // so an unsupported database is never rewritten just by being opened.
  const std::string after = pt_test::read_all(db.path);
  CHECK(after.size() == before.size());
  CHECK(after == before);
  CHECK(pt_test::read_all(db.path + "-wal").empty());
}

TEST_CASE("A well-formed v3 database is still accepted",
          "[storage][blocker][schema]") {
  // The tightened validation must not reject databases PlayerTrace itself
  // wrote: create one, close it, and reopen.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  {
    auto sink = std::make_shared<pt_test::CollectingSink>();
    auto created = Client::create(blocker_config(db.path, sink));
    REQUIRE(created.ok());
    auto client = std::move(created.client);
    REQUIRE(client->start_session("p1").ok());
    REQUIRE(client->flush(10s).ok());
    client->shutdown(5s);
  }
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto reopened = Client::create(blocker_config(db.path, sink));
  INFO(reopened.status.message());
  CHECK(reopened.ok());
  if (reopened.client) {
    reopened.client->shutdown(5s);
  }
}

// ---------------------------------------------------------------------------
// Blocker 4 — per-token flush outcomes
// ---------------------------------------------------------------------------

namespace {

/// A sink whose success can be toggled between flush passes.
class ToggleSink : public playertrace::Sink {
 public:
  Status write(const playertrace::EventBatch& batch) override {
    if (fail_.load()) {
      return Status(ErrorCode::SinkError, "toggle: refusing");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& e : batch.events) {
      events_.push_back(e);
    }
    return Status();
  }
  Status flush() override { return Status(); }
  void set_failing(bool v) { fail_.store(v); }
  std::size_t count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.size();
  }

 private:
  std::atomic<bool> fail_{true};
  mutable std::mutex mutex_;
  std::vector<playertrace::SerializedEvent> events_;
};

}  // namespace

TEST_CASE("Each flush token reports its own outcome, not the latest pass",
          "[flush][blocker][concurrency]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<ToggleSink>();
  Config config = blocker_config(db.path, sink);
  config.batch_interval = 10ms;
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  REQUIRE(client->track("e1").ok());

  // Token 1 is submitted while the sink refuses. Its pass therefore fails.
  sink->set_failing(true);
  const std::uint64_t token1 =
      playertrace::ClientInternal::submit_flush(*client);

  // Drive pass 1 to completion WITHOUT reading token1's result yet: a second
  // flush that succeeds must not be able to answer for the first.
  Status probe;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (playertrace::ClientInternal::wait_flush(*client, token1, 50ms,
                                                &probe) &&
        !probe.ok()) {
      break;  // pass 1 has completed and recorded a failure
    }
  }
  REQUIRE_FALSE(probe.ok());

  // Now repair the sink and run a SECOND flush that succeeds.
  sink->set_failing(false);
  const Status second = client->flush(10s);
  INFO("second flush: " << second.message());
  CHECK(second.ok());

  // Reading token 1 AFTER the later, successful pass must still give token 1's
  // own failure. A single most-recent-status slot returned Ok here.
  Status first_outcome;
  REQUIRE(playertrace::ClientInternal::wait_flush(*client, token1, 5s,
                                                  &first_outcome));
  INFO("token1 outcome after a later successful pass: "
       << first_outcome.message());
  CHECK_FALSE(first_outcome.ok());
  CHECK(first_outcome.code() == ErrorCode::SinkError);

  client->shutdown(5s);
}

namespace {

/// Blocks inside write() until released, then FAILS the batch, so the event
/// stays durable and unsent. That is the state in which a flush must never
/// claim success.
class BlockingFailSink : public playertrace::Sink {
 public:
  Status write(const playertrace::EventBatch&) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      entered_ = true;
    }
    entered_cv_.notify_all();
    std::unique_lock<std::mutex> lock(mutex_);
    released_cv_.wait(lock, [this] { return released_; });
    return Status(ErrorCode::SinkError, "blocking-fail: refusing");
  }
  Status flush() override { return Status(); }
  // Deliberately NOT cooperative: request_cancel() does not release it. That is
  // what makes shutdown(0) genuinely time out with the worker still inside
  // write(), which is the state this scenario needs. The test releases it
  // explicitly, and so does the destructor.
  bool wait_until_entered(std::chrono::milliseconds t) {
    std::unique_lock<std::mutex> lock(mutex_);
    return entered_cv_.wait_for(lock, t, [this] { return entered_; });
  }
  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    released_cv_.notify_all();
  }
  ~BlockingFailSink() override { release(); }

 private:
  std::mutex mutex_;
  std::condition_variable entered_cv_;
  std::condition_variable released_cv_;
  bool entered_ = false;
  bool released_ = false;
};

}  // namespace

TEST_CASE("A flush after a timed-out shutdown never reports Ok",
          "[flush][blocker][shutdown]") {
  // shutdown(0) closes admission and times out; the worker then exits with a
  // durable unsent event still pending. A later public flush() must refuse,
  // because it can verify nothing. It previously returned Ok — a success
  // manufactured purely from the worker's clean exit.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<BlockingFailSink>();
  Config config = blocker_config(db.path, sink);
  config.batch_interval = 5ms;
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  REQUIRE(client->track("stuck").ok());
  REQUIRE(sink->wait_until_entered(10s));  // worker parked inside write()

  const Status stopped = client->shutdown(0ms);
  INFO("shutdown: " << stopped.message());
  CHECK(stopped.code() == ErrorCode::Timeout);

  sink->release();
  // Wait for the worker to exit WITHOUT calling wait_for_worker_exit(): that
  // helper promotes the lifecycle to Stopped, which the old flush() did reject.
  // The defect lives in the Stopping state, so the client is left there.
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!playertrace::ClientInternal::worker_finished(*client) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
  }
  REQUIRE(playertrace::ClientInternal::worker_finished(*client));

  // Durable, unsent data is still there...
  const std::size_t pending =
      playertrace::ClientInternal::pending_in_store(*client);
  INFO("pending in store: " << pending);
  CHECK(pending >= 1);

  // ...so this flush must NOT claim success.
  const Status flushed = client->flush(5s);
  INFO("flush after shutdown: " << flushed.message());
  CHECK_FALSE(flushed.ok());
  CHECK(flushed.code() == ErrorCode::AlreadyShutdown);
}

TEST_CASE("A flush racing shutdown never reports Ok for unflushed data",
          "[flush][blocker][shutdown][concurrency]") {
  // flush() and shutdown() are driven concurrently while the sink is blocked,
  // so the lifecycle check and the token issuance can interleave. Whatever the
  // interleaving, flush must never return Ok while data is still pending.
  // Catch2 assertions run only on the main thread, after the join.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<BlockingFailSink>();
  Config config = blocker_config(db.path, sink);
  config.batch_interval = 5ms;
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  REQUIRE(client->track("stuck").ok());
  REQUIRE(sink->wait_until_entered(10s));

  std::atomic<int> flush_code{-1};
  std::atomic<bool> flush_ok{false};
  std::thread flusher([&] {
    const Status s = client->flush(5s);
    flush_ok.store(s.ok());
    flush_code.store(static_cast<int>(s.code()));
  });

  const Status stopped = client->shutdown(200ms);
  sink->release();
  flusher.join();
  client->wait_for_worker_exit(10s);

  const std::size_t pending =
      playertrace::ClientInternal::pending_in_store(*client);
  INFO("shutdown=" << stopped.message() << " flush_code=" << flush_code.load()
                   << " pending=" << pending);

  // REQUIRE the setup first. Asserting the real property only "if (pending >
  // 0)" meant the test could pass vacuously whenever the intended state — a
  // durable unsent event — failed to materialise, quietly proving nothing.
  REQUIRE(pending >= 1);

  // With data provably still pending, the flush must NOT have claimed success,
  // whichever way the lifecycle check and the token issuance interleaved.
  CHECK_FALSE(flush_ok.load());
}

TEST_CASE("A public flush returning Ok leaves nothing admitted still pending",
          "[flush][blocker]") {
  // The positive invariant behind the two refusals above: when flush() DOES
  // report Ok, every event admitted before it must be gone from the store.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = blocker_config(db.path, sink);
  config.batch_interval = 5ms;
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  for (int i = 0; i < 25; ++i) {
    REQUIRE(client->track("e" + std::to_string(i)).ok());
  }
  const Status flushed = client->flush(20s);
  REQUIRE(flushed.ok());
  CHECK(playertrace::ClientInternal::pending_in_store(*client) == 0);
  CHECK(sink->count() == 26);  // session_start + 25

  CHECK(client->shutdown(5s).ok());
  // After a completed shutdown the client is Stopped, so flush refuses.
  const Status after = client->flush(1s);
  CHECK_FALSE(after.ok());
  CHECK(after.code() == ErrorCode::AlreadyShutdown);
}

TEST_CASE("An expired flush token reports expiry, never a newer flush's result",
          "[flush][blocker]") {
  // Retention is bounded, so an old token's record is eventually discarded.
  // When that happens the lookup must say so. A nearest-match search slid
  // forward onto a NEWER token's entry and reported that unrelated flush's
  // status as if it belonged to this one — an Ok that was never earned.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<ToggleSink>();
  Config config = blocker_config(db.path, sink);
  config.batch_interval = 1ms;
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  REQUIRE(client->track("e1").ok());

  // Token 1's pass fails: the sink refuses. That is the EARLIER outcome.
  sink->set_failing(true);
  const std::uint64_t token1 =
      playertrace::ClientInternal::submit_flush(*client);
  Status early;
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (playertrace::ClientInternal::wait_flush(*client, token1, 50ms,
                                                &early) &&
        !early.ok()) {
      break;
    }
  }
  REQUIRE_FALSE(early.ok());
  REQUIRE(early.code() == ErrorCode::SinkError);

  // Now drive well past the 256-record retention limit with a DIFFERENT, and
  // successful, outcome so any slide-forward would land on an Ok.
  sink->set_failing(false);
  for (int i = 0; i < 400; ++i) {
    const std::uint64_t t = playertrace::ClientInternal::submit_flush(*client);
    Status ignored;
    playertrace::ClientInternal::wait_flush(*client, t, 10s, &ignored);
  }

  // Token 1's record is now long gone. Reading it must be an explicit,
  // truthful failure — and specifically NOT the later passes' success.
  Status expired;
  REQUIRE(
      playertrace::ClientInternal::wait_flush(*client, token1, 5s, &expired));
  INFO("token1 after 400 later flushes: " << expired.message());
  CHECK_FALSE(expired.ok());
  CHECK(expired.message().find("no longer available") != std::string::npos);

  // A recent token still reports its own real outcome.
  const std::uint64_t recent =
      playertrace::ClientInternal::submit_flush(*client);
  Status fresh;
  REQUIRE(
      playertrace::ClientInternal::wait_flush(*client, recent, 10s, &fresh));
  CHECK(fresh.ok());

  client->shutdown(5s);
}

TEST_CASE("Abandoned flush tokens do not accumulate completion records",
          "[flush][blocker]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = blocker_config(db.path, sink);
  config.batch_interval = 5ms;
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  REQUIRE(client->start_session("p1").ok());

  // Submit far more tokens than the retention bound and never wait on most of
  // them. Retention is bounded, so this must not grow without limit; the client
  // must also still work normally afterwards.
  for (int i = 0; i < 500; ++i) {
    const std::uint64_t token =
        playertrace::ClientInternal::submit_flush(*client);
    if (i % 100 == 0) {
      Status ignored;
      playertrace::ClientInternal::wait_flush(*client, token, 0ms, &ignored);
    }
  }
  REQUIRE(client->track("still_works").ok());
  CHECK(client->flush(10s).ok());
  CHECK(sink->count() == 2);
  client->shutdown(5s);
}
