// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// FileSink durability and failure handling (audit finding #7). The previous
// suite contained no FileSink coverage at all.
#include <catch2/catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "internal/client_internal.hpp"
#include "internal/file_sink_internal.hpp"
#include "playertrace/playertrace.hpp"
#include "test_support.hpp"

using namespace std::chrono_literals;
using playertrace::Client;
using playertrace::Config;
using playertrace::ConsentState;
using playertrace::ErrorCode;
using playertrace::EventBatch;
using playertrace::FileSink;
using playertrace::FileSinkInternal;
using playertrace::SerializedEvent;

namespace {

EventBatch batch_of(const std::vector<std::string>& ids) {
  EventBatch batch;
  for (const auto& id : ids) {
    batch.events.push_back(
        SerializedEvent{id, "{\"event_id\":\"" + id + "\",\"n\":1}"});
  }
  return batch;
}

/// True if every non-empty line in the file parses as one JSON object.
bool every_line_is_json(const std::string& path) {
  for (const auto& line : pt_test::read_lines(path)) {
    if (!nlohmann::json::accept(line)) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("FileSink writes one JSON object per line", "[file_sink]") {
  pt_test::ScopedFile out(pt_test::unique_out_path());
  FileSink sink(out.path);
  REQUIRE(sink.write(batch_of({"a", "b", "c"})).ok());
  REQUIRE(sink.flush().ok());

  const auto lines = pt_test::read_lines(out.path);
  CHECK(lines.size() == 3);
  CHECK(every_line_is_json(out.path));
  CHECK(nlohmann::json::parse(lines[0]).at("event_id") == "a");
  CHECK(nlohmann::json::parse(lines[2]).at("event_id") == "c");
  CHECK(sink.path() == out.path);
}

TEST_CASE("FileSink appends across reopens", "[file_sink]") {
  pt_test::ScopedFile out(pt_test::unique_out_path());
  {
    FileSink sink(out.path);
    REQUIRE(sink.write(batch_of({"a"})).ok());
  }
  {
    FileSink sink(out.path);
    REQUIRE(sink.write(batch_of({"b"})).ok());
  }
  const auto lines = pt_test::read_lines(out.path);
  CHECK(lines.size() == 2);
  CHECK(every_line_is_json(out.path));
}

TEST_CASE("An unopenable path reports SinkError", "[file_sink]") {
  // A path inside a non-existent directory cannot be opened for append.
  FileSink sink("pt_missing_dir_xyz/nested/out.ndjson");
  const auto status = sink.write(batch_of({"a"}));
  CHECK_FALSE(status.ok());
  CHECK(status.code() == ErrorCode::SinkError);
}

TEST_CASE("Injected open failures report SinkError", "[file_sink][fault]") {
  pt_test::ScopedFile out(pt_test::unique_out_path());
  FileSink sink(out.path);
  FileSinkInternal::set_fault_hook(
      sink, [](const char* op) { return std::string(op) == "open"; });
  CHECK(sink.write(batch_of({"a"})).code() == ErrorCode::SinkError);

  // Clearing the fault must let it recover: the sink is not poisoned.
  FileSinkInternal::set_fault_hook(sink, nullptr);
  CHECK(sink.write(batch_of({"a"})).ok());
  CHECK(pt_test::read_lines(out.path).size() == 1);
}

TEST_CASE("A partial write is rolled back to the last good line",
          "[file_sink][fault]") {
  pt_test::ScopedFile out(pt_test::unique_out_path());
  FileSink sink(out.path);
  REQUIRE(sink.write(batch_of({"first"})).ok());
  const std::string after_first = pt_test::read_all(out.path);

  FileSinkInternal::set_fault_hook(
      sink, [](const char* op) { return std::string(op) == "partial_write"; });
  CHECK(sink.write(batch_of({"torn"})).code() == ErrorCode::SinkError);
  FileSinkInternal::set_fault_hook(sink, nullptr);

  // The torn fragment must be gone; the file is exactly as it was.
  CHECK(pt_test::read_all(out.path) == after_first);
  CHECK(every_line_is_json(out.path));

  // A retry then produces clean NDJSON, not a fragment followed by a record.
  REQUIRE(sink.write(batch_of({"torn"})).ok());
  const auto lines = pt_test::read_lines(out.path);
  CHECK(lines.size() == 2);
  CHECK(every_line_is_json(out.path));
  CHECK(nlohmann::json::parse(lines[1]).at("event_id") == "torn");
}

TEST_CASE("Write, flush and sync failures all roll back",
          "[file_sink][fault]") {
  const std::string op =
      GENERATE(std::string("write"), std::string("flush"), std::string("sync"));
  CAPTURE(op);

  pt_test::ScopedFile out(pt_test::unique_out_path());
  FileSink sink(out.path);
  REQUIRE(sink.write(batch_of({"kept"})).ok());
  const std::string baseline = pt_test::read_all(out.path);

  FileSinkInternal::set_fault_hook(sink,
                                   [op](const char* o) { return op == o; });
  CHECK(sink.write(batch_of({"lost"})).code() == ErrorCode::SinkError);
  FileSinkInternal::set_fault_hook(sink, nullptr);

  CHECK(pt_test::read_all(out.path) == baseline);
  CHECK(every_line_is_json(out.path));

  REQUIRE(sink.write(batch_of({"lost"})).ok());  // recovers cleanly
  CHECK(pt_test::read_lines(out.path).size() == 2);
}

TEST_CASE("A failed rollback fails the sink permanently",
          "[file_sink][fault]") {
  // If the rollback itself cannot complete, the file's contents can no longer
  // be reasoned about. Continuing to append would risk writing after a torn
  // line, so the sink stays failed and the events stay in durable storage.
  pt_test::ScopedFile out(pt_test::unique_out_path());
  FileSink sink(out.path);
  REQUIRE(sink.write(batch_of({"a"})).ok());

  FileSinkInternal::set_fault_hook(sink, [](const char* op) {
    const std::string o(op);
    return o == "partial_write" || o == "truncate";
  });
  const auto status = sink.write(batch_of({"b"}));
  CHECK_FALSE(status.ok());
  CHECK(status.code() == ErrorCode::SinkError);
  FileSinkInternal::set_fault_hook(sink, nullptr);

  // Even with the faults cleared, the sink refuses further writes.
  CHECK_FALSE(sink.failed().ok());
  const auto later = sink.write(batch_of({"c"}));
  CHECK_FALSE(later.ok());
  CHECK(later.code() == ErrorCode::SinkError);
}

TEST_CASE("A reopened file is not truncated by a first-write failure",
          "[file_sink][fault][regression]") {
  // REGRESSION: ftell() returns 0 immediately after fopen(path, "ab") because
  // the append position is not established until the first write. Using that
  // as the rollback point truncated the whole file, destroying events that had
  // already been acknowledged and deleted from SQLite. The rollback point must
  // come from an explicit seek to the end.
  pt_test::ScopedFile out(pt_test::unique_out_path());
  {
    FileSink first(out.path);
    REQUIRE(first.write(batch_of({"already", "acknowledged"})).ok());
  }
  const std::string before = pt_test::read_all(out.path);
  REQUIRE(pt_test::read_lines(out.path).size() == 2);
  REQUIRE(before.size() > 0);

  // A brand new sink on the existing file: the very FIRST write fails.
  FileSink reopened(out.path);
  FileSinkInternal::set_fault_hook(
      reopened, [](const char* op) { return std::string(op) == "write"; });
  CHECK(reopened.write(batch_of({"doomed"})).code() == ErrorCode::SinkError);
  FileSinkInternal::set_fault_hook(reopened, nullptr);

  // The previously acknowledged output must be exactly as it was.
  CHECK(pt_test::read_all(out.path) == before);
  CHECK(pt_test::read_lines(out.path).size() == 2);
  CHECK(every_line_is_json(out.path));

  // ...and the sink still works afterwards.
  REQUIRE(reopened.write(batch_of({"later"})).ok());
  CHECK(pt_test::read_lines(out.path).size() == 3);
  CHECK(every_line_is_json(out.path));
}

TEST_CASE("A torn final line from a crash is repaired on reopen",
          "[file_sink][regression]") {
  // A process killed mid-write leaves a line with no newline. Appending after
  // it would create a permanently unparseable line.
  pt_test::ScopedFile out(pt_test::unique_out_path());
  {
    FileSink first(out.path);
    REQUIRE(first.write(batch_of({"complete"})).ok());
  }
  // Simulate the torn tail.
  {
    std::ofstream raw(out.path, std::ios::app | std::ios::binary);
    raw << "{\"event_id\":\"tor";
  }
  REQUIRE(pt_test::read_all(out.path).find("tor") != std::string::npos);

  FileSink reopened(out.path);
  REQUIRE(reopened.write(batch_of({"after_crash"})).ok());

  const auto lines = pt_test::read_lines(out.path);
  CHECK(lines.size() == 2);  // the fragment was removed, not appended to
  CHECK(every_line_is_json(out.path));
  CHECK(nlohmann::json::parse(lines[0]).at("event_id") == "complete");
  CHECK(nlohmann::json::parse(lines[1]).at("event_id") == "after_crash");
}

TEST_CASE("Two FileSinks cannot own the same path", "[file_sink][regression]") {
  // Independent mutexes would interleave writes, and one instance's rollback
  // could truncate the other's successfully written output.
  pt_test::ScopedFile out(pt_test::unique_out_path());
  FileSink first(out.path);
  CHECK(first.failed().ok());

  FileSink second(out.path);
  CHECK_FALSE(second.failed().ok());
  CHECK(second.write(batch_of({"a"})).code() == ErrorCode::InvalidConfig);

  // The first sink is unaffected and still works.
  CHECK(first.write(batch_of({"a"})).ok());
}

TEST_CASE("Sink::flush failures propagate", "[file_sink][fault]") {
  pt_test::ScopedFile out(pt_test::unique_out_path());
  FileSink sink(out.path);
  REQUIRE(sink.write(batch_of({"a"})).ok());
  FileSinkInternal::set_fault_hook(
      sink, [](const char* op) { return std::string(op) == "sync"; });
  CHECK(sink.flush().code() == ErrorCode::SinkError);
  FileSinkInternal::set_fault_hook(sink, nullptr);
  CHECK(sink.flush().ok());
}

TEST_CASE("Durable sync can be disabled explicitly", "[file_sink]") {
  pt_test::ScopedFile out(pt_test::unique_out_path());
  FileSink sink(out.path, /*durable_sync=*/false);
  // With sync disabled the sync fault must never be consulted.
  FileSinkInternal::set_fault_hook(
      sink, [](const char* op) { return std::string(op) == "sync"; });
  CHECK(sink.write(batch_of({"a"})).ok());
  FileSinkInternal::set_fault_hook(sink, nullptr);
  CHECK(pt_test::read_lines(out.path).size() == 1);
}

// ---------------------------------------------------------------------------
// FileSink inside the full pipeline
// ---------------------------------------------------------------------------

TEST_CASE("A FileSink failure retains events for a later retry",
          "[file_sink][client]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  pt_test::ScopedFile out(pt_test::unique_out_path());
  auto sink = std::make_shared<FileSink>(out.path);

  Config config;
  config.app_id = "file-sink-test";
  config.storage_path = db.path;
  config.consent = ConsentState::Granted;
  config.sink = sink;
  config.batch_interval = std::chrono::minutes(10);

  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  REQUIRE(client->track("blocked").ok());

  FileSinkInternal::set_fault_hook(
      *sink, [](const char* op) { return std::string(op) == "write"; });
  client->flush(5s);
  FileSinkInternal::set_fault_hook(*sink, nullptr);

  // Nothing acknowledged, so nothing was deleted from durable storage.
  CHECK(pt_test::read_lines(out.path).empty());
  CHECK(playertrace::ClientInternal::pending_in_store(*client) == 2);

  REQUIRE(client->flush(10s).ok());
  const auto lines = pt_test::read_lines(out.path);
  CHECK(lines.size() == 2);
  CHECK(every_line_is_json(out.path));
  CHECK(playertrace::ClientInternal::pending_in_store(*client) == 0);
}

TEST_CASE("Every line written through the pipeline is valid NDJSON",
          "[file_sink][client]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  pt_test::ScopedFile out(pt_test::unique_out_path());
  auto sink = std::make_shared<FileSink>(out.path);

  Config config;
  config.app_id = "file-sink-test";
  config.storage_path = db.path;
  config.consent = ConsentState::Granted;
  config.sink = sink;
  config.batch_interval = std::chrono::minutes(10);

  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  for (int i = 0; i < 25; ++i) {
    REQUIRE(client
                ->track("evt", {{"i", std::int64_t{i}},
                                {"text", std::string("caf\xC3\xA9")}})
                .ok());
  }
  client->end_session();
  REQUIRE(client->flush(10s).ok());
  REQUIRE(client->shutdown(5s).ok());

  const auto lines = pt_test::read_lines(out.path);
  CHECK(lines.size() == 27);  // start + 25 + end
  CHECK(every_line_is_json(out.path));
  // Non-ASCII survives the round trip intact.
  CHECK(nlohmann::json::parse(lines[1]).at("properties").at("text") ==
        "caf\xC3\xA9");
}
