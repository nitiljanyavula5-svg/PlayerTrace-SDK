// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Regressions carried over from the independent re-audit (findings A–D).
//
//  A  start_session() replacing a live session must be atomic. Its session_end
//     CANNOT be un-admitted, so it must not be emitted until the replacement
//     session_start is known to fit as well — and an ended session must never
//     be restored, which previously reused (session_id, seq).
//  B  When the worker exits unexpectedly, admission closes permanently.
//  D  A path is owned by exactly one component, whatever it uses it for; and
//     SQLite in-memory URIs are parsed rather than substring-matched.
//
// Determinism comes from latches and observable counters, never from sleeping.
#include <catch2/catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "internal/client_internal.hpp"
#include "internal/path_registry.hpp"
#include "playertrace/file_sink.hpp"
#include "playertrace/playertrace.hpp"
#include "test_support.hpp"

using namespace std::chrono_literals;
using playertrace::Client;
using playertrace::Config;
using playertrace::ConsentState;
using playertrace::ErrorCode;
using playertrace::Status;

namespace {

Config regression_config(const std::string& path,
                         std::shared_ptr<playertrace::Sink> sink) {
  Config c;
  c.app_id = "regress";
  c.storage_path = path;
  c.consent = ConsentState::Granted;
  c.sink = std::move(sink);
  // A long interval plus a batch threshold these tests never reach parks the
  // worker in pop_wait, so nothing drains until the test kicks it with flush().
  c.batch_interval = std::chrono::minutes(10);
  c.batch_size = 100;
  c.max_queue_size = 1000;
  return c;
}

struct Marker {
  std::string name;
  std::string session_id;
  std::uint64_t seq;
};

std::vector<Marker> markers_of(const pt_test::CollectingSink& sink) {
  std::vector<Marker> out;
  for (const auto& e : sink.events()) {
    const auto j = nlohmann::json::parse(e.json);
    out.push_back(Marker{j.at("name").get<std::string>(),
                         j.at("session_id").get<std::string>(),
                         j.at("seq").get<std::uint64_t>()});
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// A — atomic session replacement
// ---------------------------------------------------------------------------

TEST_CASE("A failed session replacement leaves the existing session untouched",
          "[client][session][regression]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = regression_config(db.path, sink);
  // Exactly one free slot at replacement time: session_end would fit, but the
  // session_start that must follow it would not.
  config.max_pending_events = 2;
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());

  const Status replaced = client->start_session("p2");
  REQUIRE_FALSE(replaced.ok());
  CHECK(replaced.code() == ErrorCode::StorageFull);

  // The caller still has the session they started with, and it is still usable.
  // Only the two-event replacement was refused.
  REQUIRE(client->flush(10s).ok());
  const Status tracked = client->track("still_running");
  INFO("track after a refused replacement: " << tracked.message());
  CHECK(tracked.ok());
  CHECK(tracked.code() != ErrorCode::NotStarted);
  CHECK(client->end_session().ok());
  REQUIRE(client->flush(10s).ok());

  const auto seen = markers_of(*sink);
  REQUIRE(seen.size() == 3);
  CHECK(seen[0].name == "session_start");
  CHECK(seen[1].name == "still_running");
  CHECK(seen[2].name == "session_end");
  // One session, one contiguous sequence. Before the fix the refused
  // replacement had already admitted a session_end and then restored the
  // session with its counter rewound, so "still_running" reused seq 1 and
  // arrived after that session_end.
  CHECK(seen[0].session_id == seen[1].session_id);
  CHECK(seen[1].session_id == seen[2].session_id);
  CHECK(seen[0].seq == 0);
  CHECK(seen[1].seq == 1);
  CHECK(seen[2].seq == 2);
}

TEST_CASE("A session replacement that fits is still a clean handover",
          "[client][session][regression]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = regression_config(db.path, sink);
  config.max_pending_events = 3;  // room for session_end + session_start
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  REQUIRE(client->start_session("p2").ok());
  REQUIRE(client->flush(10s).ok());

  const auto seen = markers_of(*sink);
  REQUIRE(seen.size() == 3);
  CHECK(seen[0].name == "session_start");
  CHECK(seen[1].name == "session_end");
  CHECK(seen[2].name == "session_start");
  CHECK(seen[0].session_id == seen[1].session_id);
  CHECK(seen[2].session_id != seen[0].session_id);
  CHECK(seen[2].seq == 0);  // the new session restarts numbering
}

TEST_CASE("No accepted event is ever delivered after its own session_end",
          "[client][session][regression]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = regression_config(db.path, sink);
  config.max_pending_events = 2;
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  CHECK_FALSE(client->start_session("p2").ok());
  REQUIRE(client->flush(10s).ok());
  client->track("more");
  client->end_session();
  REQUIRE(client->flush(10s).ok());

  std::set<std::string> ended;
  std::set<std::string> keys;
  for (const auto& m : markers_of(*sink)) {
    INFO("event " << m.name << " seq " << m.seq);
    CHECK(ended.count(m.session_id) == 0);  // nothing after a session_end
    CHECK(keys.insert(m.session_id + "#" + std::to_string(m.seq)).second);
    if (m.name == "session_end") {
      ended.insert(m.session_id);
    }
  }
}

// ---------------------------------------------------------------------------
// B — admission closes when the worker dies unexpectedly
// ---------------------------------------------------------------------------

namespace {

/// Kills the worker deterministically. A fault hook that THROWS escapes
/// SqliteStore::insert and reaches the worker's catch-all, which is exactly the
/// "unexpected exit" path; returning true would only be a reported error.
void kill_worker(Client& client) {
  playertrace::ClientInternal::set_store_fault_hook(
      client, [](const char*) -> bool {
        throw std::runtime_error("regression: injected worker-fatal fault");
      });
  const Status flushed = client.flush(10s);
  REQUIRE_FALSE(flushed.ok());
  REQUIRE(flushed.code() == ErrorCode::Internal);
}

}  // namespace

TEST_CASE("A worker that dies unexpectedly closes admission for good",
          "[client][lifecycle][regression]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = regression_config(db.path, sink);
  config.max_pending_events = 100000;  // the cap must not be what stops us
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  REQUIRE(client->track("before").ok());

  kill_worker(*client);

  // Every admitting entry point must now report the terminal failure. Before
  // the fix each of these returned Ok forever, into a queue nothing drains.
  const Status tracked = client->track("after");
  CHECK_FALSE(tracked.ok());
  CHECK(tracked.code() == ErrorCode::Internal);

  CHECK_FALSE(client->track("after_again").ok());
  CHECK_FALSE(client->start_session("p2").ok());
  CHECK_FALSE(client->end_session().ok());
  CHECK_FALSE(client->track("later_still").ok());  // stays closed

  const auto stats = playertrace::ClientInternal::stats(*client);
  CHECK(stats.accepted == 2);  // session_start + "before", nothing more

  playertrace::ClientInternal::set_store_fault_hook(*client, nullptr);
  const Status stopped = client->shutdown(5s);
  CHECK(stopped.code() == ErrorCode::Internal);
}

TEST_CASE("A dead worker does not block consent revocation",
          "[client][consent][regression]") {
  // Revocation is synchronous and must keep working: it is a privacy
  // obligation, not an admission.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(regression_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  REQUIRE(client->track("secret").ok());
  kill_worker(*client);
  playertrace::ClientInternal::set_store_fault_hook(*client, nullptr);

  CHECK(client->set_consent(ConsentState::Denied).ok());
  CHECK(client->consent() == ConsentState::Denied);
  CHECK(playertrace::ClientInternal::pending_in_store(*client) == 0);
}

// ---------------------------------------------------------------------------
// D — one owner per path; in-memory URIs are parsed, not substring-matched
// ---------------------------------------------------------------------------

TEST_CASE("A FileSink cannot claim the file backing the SQLite store",
          "[client][storage][regression]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  // The sink is constructed first, so it legitimately wins the claim...
  auto sink = std::make_shared<playertrace::FileSink>(db.path);
  CHECK(sink->failed().ok());

  // ...and the store must then refuse the path instead of opening a database
  // the sink is about to append NDJSON into.
  auto created = Client::create(regression_config(db.path, sink));
  CHECK_FALSE(created.ok());
  CHECK(created.status.code() == ErrorCode::InvalidConfig);
  CHECK(created.client == nullptr);

  // The reverse order must be caught too: store first, sink second.
  pt_test::ScopedDb db2(pt_test::unique_db_path());
  auto plain = std::make_shared<pt_test::CollectingSink>();
  auto owner = Client::create(regression_config(db2.path, plain));
  REQUIRE(owner.ok());
  auto late_sink = std::make_shared<playertrace::FileSink>(db2.path);
  CHECK_FALSE(late_sink->failed().ok());
  CHECK(owner.client->shutdown(5s).ok());
}

TEST_CASE("Distinct storage and output paths are both claimable",
          "[client][storage][regression]") {
  // The single namespace must not create false conflicts between genuinely
  // different files — that is the normal configuration.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  pt_test::ScopedFile out(pt_test::unique_out_path());
  auto sink = std::make_shared<playertrace::FileSink>(out.path);
  CHECK(sink->failed().ok());

  auto created = Client::create(regression_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  REQUIRE(client->start_session("p1").ok());
  CHECK(client->flush(10s).ok());
  CHECK(client->shutdown(5s).ok());
}

TEST_CASE("SQLite in-memory URIs are parsed, not substring-matched",
          "[storage][regression]") {
  using playertrace::internal::PathClaim;
  using playertrace::internal::PathKind;
  std::string error;

  // The exemption must describe what THIS BUILD's SQLite actually does. URI
  // filenames require SQLITE_USE_URI or SQLITE_OPEN_URI, and PlayerTrace
  // enables neither, so every "file:..." string below names an ORDINARY DISK
  // FILE and must be exclusively owned. Only ":memory:" is per-connection.

  // A real file whose name merely contains the token must still be claimed.
  const std::string tricky =
      "pt_mode=memory_" + pt_test::process_token() + ".ndjson";
  PathClaim first;
  PathClaim second;
  CHECK(first.acquire(tricky, PathKind::Output, &error));
  CHECK_FALSE(second.acquire(tricky, PathKind::Output, &error));

  // ...likewise a file literally named ":memory:.db".
  const std::string near_miss =
      "pt_memory_" + pt_test::process_token() + ":memory:.db";
  PathClaim n1;
  PathClaim n2;
  CHECK(n1.acquire(near_miss, PathKind::Storage, &error));
  CHECK_FALSE(n2.acquire(near_miss, PathKind::Storage, &error));

  // Every URI-looking spelling is a real file here, so each is owned
  // exclusively. Exempting them handed out an exemption SQLite never honoured
  // and let two clients share one database undetected.
  const std::string tok = pt_test::process_token();
  const std::string uris[] = {"file::memory:" + tok,
                              "file:" + tok + "?mode=memory",
                              "file:" + tok + "?cache=shared&mode=memory",
                              "file:" + tok + ".db?mode=memoryx"};
  for (const std::string& uri : uris) {
    PathClaim a;
    PathClaim b;
    INFO("uri " << uri);
    CHECK(a.acquire(uri, PathKind::Storage, &error));
    CHECK_FALSE(b.acquire(uri, PathKind::Storage, &error));
  }

  // The one genuine in-memory spelling stays non-exclusive: each connection
  // gets a private database, so there is nothing to own.
  PathClaim m1;
  PathClaim m2;
  CHECK(m1.acquire(":memory:", PathKind::Storage, &error));
  CHECK(m2.acquire(":memory:", PathKind::Storage, &error));
}

TEST_CASE("Two clients cannot share a database named like a memory URI",
          "[client][storage][regression]") {
  // End-to-end counterpart: the collision must be caught through the public
  // factory, not only at the PathClaim level.
  pt_test::ScopedDb db("pt_modememory_" + pt_test::process_token() +
                       "_mode=memory.db");
  auto sink_a = std::make_shared<pt_test::CollectingSink>();
  auto first = Client::create(regression_config(db.path, sink_a));
  REQUIRE(first.ok());

  auto sink_b = std::make_shared<pt_test::CollectingSink>();
  auto second = Client::create(regression_config(db.path, sink_b));
  INFO(second.status.message());
  CHECK_FALSE(second.ok());
  CHECK(second.status.code() == ErrorCode::InvalidConfig);
  CHECK(second.client == nullptr);

  CHECK(first.client->shutdown(5s).ok());
}

// ---------------------------------------------------------------------------
// C — storage-fault retry behaviour
//
// The merged tree's worker always blocks in pop_wait at the top of its loop and
// takes the retry buffer inside process_incoming, so the busy-spin these guard
// against is prevented by construction rather than by a backoff. They are kept
// as regressions so a future refactor cannot quietly reintroduce it.
// ---------------------------------------------------------------------------

TEST_CASE("A resolved storage fault does not fail the next flush",
          "[persistence][flush][regression]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = regression_config(db.path, sink);
  config.batch_interval = 10ms;
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  REQUIRE(client->track("one").ok());
  REQUIRE(client->track("two").ok());

  std::atomic<long> attempts{0};
  playertrace::ClientInternal::set_store_fault_hook(
      *client, [&attempts](const char* op) {
        attempts.fetch_add(1);
        return std::string(op) == "commit";
      });

  const Status faulted = client->flush(5s);
  CHECK_FALSE(faulted.ok());
  CHECK(faulted.code() == ErrorCode::StorageError);

  // Let the worker retry, and keep failing, while the fault is installed — so
  // errors really are recorded between the two flushes. Waiting on the counter
  // rather than the clock is what makes this deterministic.
  const long baseline = attempts.load();
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (attempts.load() < baseline + 3 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  REQUIRE(attempts.load() >= baseline + 3);

  playertrace::ClientInternal::set_store_fault_hook(*client, nullptr);

  // The events were only ever deferred, never lost. Once the fault clears the
  // flush must succeed: an error that has already been resolved by a retry
  // must not be charged to a later, unrelated flush.
  const Status recovered = client->flush(10s);
  INFO("recovered flush: " << recovered.message());
  CHECK(recovered.ok());

  const auto seen = markers_of(*sink);
  REQUIRE(seen.size() == 3);
  CHECK(seen[0].name == "session_start");
  CHECK(seen[1].name == "one");
  CHECK(seen[2].name == "two");

  const auto stats = playertrace::ClientInternal::stats(*client);
  CHECK(stats.accepted == 3);
  CHECK(stats.delivered == 3);
  client->shutdown(5s);
}

TEST_CASE("A persistent storage fault does not make the worker spin",
          "[persistence][regression]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = regression_config(db.path, sink);
  config.batch_interval = 50ms;
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  REQUIRE(client->track("one").ok());

  std::atomic<long> attempts{0};
  playertrace::ClientInternal::set_store_fault_hook(
      *client, [&attempts](const char* op) {
        attempts.fetch_add(1);
        return std::string(op) == "commit";
      });
  CHECK_FALSE(client->flush(5s).ok());

  // With no flush pending the worker waits on the queue between retries rather
  // than hammering the store. A spinning loop managed thousands of attempts in
  // this window; the batch interval bounds it to a couple of dozen.
  const long before = attempts.load();
  std::this_thread::sleep_for(500ms);
  const long spun = attempts.load() - before;
  INFO("store operations attempted in 500ms: " << spun);
  CHECK(spun < 400);

  // Backing off must not mean giving up.
  playertrace::ClientInternal::set_store_fault_hook(*client, nullptr);
  CHECK(client->flush(10s).ok());
  CHECK(markers_of(*sink).size() == 2);
  client->shutdown(5s);
}

TEST_CASE("A flush still reports a fault present during its own pass",
          "[persistence][flush][regression]") {
  // The counterpart to the scoping requirement: narrowing which errors a flush
  // inherits must not stop it reporting a genuine, still-active failure.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  Config config = regression_config(db.path, sink);
  config.batch_interval = 10ms;
  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  REQUIRE(client->start_session("p1").ok());
  REQUIRE(client->track("one").ok());

  playertrace::ClientInternal::set_store_fault_hook(
      *client, [](const char* op) { return std::string(op) == "commit"; });

  for (int attempt = 0; attempt < 3; ++attempt) {
    const Status flushed = client->flush(5s);
    INFO("attempt " << attempt << ": " << flushed.message());
    CHECK_FALSE(flushed.ok());
    CHECK(flushed.code() == ErrorCode::StorageError);
  }
  CHECK(sink->count() == 0);

  playertrace::ClientInternal::set_store_fault_hook(*client, nullptr);
  CHECK(client->flush(10s).ok());
  CHECK(sink->count() == 2);
  client->shutdown(5s);
}
