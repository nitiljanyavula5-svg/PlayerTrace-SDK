// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Shutdown, admission closure, and callback contracts (audit findings #3, #5,
// #14). Every wait here is latch-driven or bounded; none of these tests sleep
// for an arbitrary interval and hope.
#include <catch2/catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "internal/client_internal.hpp"
#include "playertrace/playertrace.hpp"
#include "test_support.hpp"

using namespace std::chrono_literals;
using playertrace::Client;
using playertrace::Config;
using playertrace::ConsentState;
using playertrace::ErrorCode;
using playertrace::LogLevel;

namespace {

Config base_config(const std::string& path,
                   std::shared_ptr<playertrace::Sink> sink) {
  Config c;
  c.app_id = "lifecycle-test";
  c.storage_path = path;
  c.consent = ConsentState::Granted;
  c.sink = std::move(sink);
  c.batch_interval = std::chrono::minutes(10);
  return c;
}

}  // namespace

TEST_CASE("shutdown returns Timeout but never abandons the worker",
          "[lifecycle][shutdown]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::BlockingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  client->track("stuck");
  // Kick the worker into the sink and wait until it is genuinely blocked.
  client->flush(200ms);
  REQUIRE(sink->wait_until_entered(10s));

  const auto start = std::chrono::steady_clock::now();
  const auto status = client->shutdown(300ms);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  // The advertised timeout is honored...
  CHECK(status.code() == ErrorCode::Timeout);
  CHECK(elapsed < 10s);
  // ...and the worker is still owned and still running. It is NOT detached:
  // it can reach state captured by the sink and callbacks.
  CHECK_FALSE(playertrace::ClientInternal::worker_finished(*client));
  CHECK_FALSE(client->wait_for_worker_exit(0ms));

  sink->release();
  // Only now can the worker actually finish.
  CHECK(client->wait_for_worker_exit(10s));
  CHECK(playertrace::ClientInternal::worker_finished(*client));
}

TEST_CASE("A cooperative sink makes shutdown bounded",
          "[lifecycle][shutdown]") {
  // request_cancel() is the contract by which a custom sink keeps shutdown
  // bounded. A sink that honors it unblocks itself.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CooperativeBlockingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  client->track("stuck");
  client->flush(200ms);
  REQUIRE(sink->wait_until_entered(10s));

  const auto start = std::chrono::steady_clock::now();
  const auto status = client->shutdown(10s);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  CAPTURE(status.message());
  CHECK(status.ok());  // reached a genuine terminal state
  CHECK(elapsed < 10s);
  CHECK(sink->saw_cancel());
  CHECK(playertrace::ClientInternal::worker_finished(*client));
}

TEST_CASE("Destruction joins a wedged sink rather than abandoning it",
          "[lifecycle][shutdown]") {
  // The destructor blocks until the worker exits: a released sink lets it
  // complete. This is the documented cost of never abandoning the thread.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::BlockingSink>();
  std::atomic<bool> destroyed{false};

  // Catch2's assertion and output state is NOT thread-safe. Asserting from a
  // spawned thread races the main thread inside Catch's output redirection,
  // which ThreadSanitizer reports — a defect in the harness, not the SDK.
  // Observations are recorded here and asserted after the join instead.
  std::atomic<bool> created_ok{false};
  std::atomic<bool> sink_entered{false};

  std::thread owner([&] {
    auto created = Client::create(base_config(db.path, sink));
    created_ok.store(created.ok());
    if (!created.ok()) {
      return;  // nothing further is meaningful; the main thread reports it
    }
    auto client = std::move(created.client);
    client->start_session("anon");
    client->track("stuck");
    client->flush(200ms);
    sink_entered.store(sink->wait_until_entered(10s));
    // Destructor runs at scope exit and must wait for the worker.
    client.reset();
    destroyed.store(true);
  });

  // The destructor must still be waiting while the sink is blocked.
  const bool main_saw_entry = sink->wait_until_entered(10s);
  sink->release();
  owner.join();

  // Every assertion runs on the main thread, after the join.
  REQUIRE(created_ok.load());
  REQUIRE(main_saw_entry);
  CHECK(sink_entered.load());
  CHECK(destroyed.load());
}

TEST_CASE("Shutdown reaches a terminal state once a slow sink completes",
          "[lifecycle][shutdown]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::BlockingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  client->track("slow");
  client->flush(200ms);
  REQUIRE(sink->wait_until_entered(10s));

  CHECK(client->shutdown(200ms).code() == ErrorCode::Timeout);
  CHECK_FALSE(playertrace::ClientInternal::worker_finished(*client));

  sink->release();
  // Only after the worker really exits does shutdown report success.
  CHECK(client->shutdown(10s).ok());
  CHECK(playertrace::ClientInternal::worker_finished(*client));
}

TEST_CASE("A later shutdown never reports Ok while the worker still runs",
          "[lifecycle][shutdown]") {
  // The old code marked the lifecycle Stopped even when the join timed out, so
  // every later caller was told Ok while the worker was still inside the sink.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::BlockingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  client->track("stuck");
  client->flush(200ms);
  REQUIRE(sink->wait_until_entered(10s));

  CHECK(client->shutdown(200ms).code() == ErrorCode::Timeout);
  // Second and third callers must see the same truth, not a stale Ok.
  CHECK(client->shutdown(200ms).code() == ErrorCode::Timeout);
  CHECK(client->shutdown(200ms).code() == ErrorCode::Timeout);

  sink->release();
  CHECK(client->shutdown(10s).ok());
}

TEST_CASE("Concurrent shutdown callers wait for the real terminal state",
          "[lifecycle][shutdown][concurrency]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  for (int i = 0; i < 50; ++i) {
    client->track("evt");
  }

  constexpr int kCallers = 6;
  std::vector<std::thread> threads;
  std::atomic<int> ok_count{0};
  std::atomic<bool> go{false};
  for (int i = 0; i < kCallers; ++i) {
    threads.emplace_back([&] {
      while (!go.load()) {
        std::this_thread::yield();
      }
      if (client->shutdown(10s).ok()) {
        ok_count.fetch_add(1);
      }
    });
  }
  go.store(true);
  for (auto& t : threads) {
    t.join();
  }

  // Every caller returns Ok, and only after the worker has actually stopped.
  CHECK(ok_count.load() == kCallers);
  CHECK(client->track("after").code() == ErrorCode::AlreadyShutdown);
  CHECK(sink->count() == 51);  // everything accepted was delivered
}

TEST_CASE("track racing shutdown never returns Ok for lost work",
          "[lifecycle][shutdown][concurrency]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  client->start_session("anon");

  std::atomic<bool> go{false};
  std::atomic<int> accepted{0};
  std::vector<std::thread> producers;
  for (int t = 0; t < 4; ++t) {
    producers.emplace_back([&] {
      while (!go.load()) {
        std::this_thread::yield();
      }
      for (int i = 0; i < 500; ++i) {
        if (client->track("racy").ok()) {
          accepted.fetch_add(1);
        }
      }
    });
  }

  go.store(true);
  std::this_thread::yield();
  REQUIRE(client->shutdown(15s).ok());
  for (auto& t : producers) {
    t.join();
  }

  // Everything that was told "Ok" is accounted for: delivered, or still
  // durable.
  auto stats = playertrace::ClientInternal::stats(*client);
  const std::size_t still_pending =
      playertrace::ClientInternal::pending_in_store(*client);
  CHECK(stats.accepted == static_cast<std::uint64_t>(accepted.load()) + 1);
  CHECK(sink->count() + still_pending ==
        static_cast<std::size_t>(accepted.load()) + 1);
}

TEST_CASE("Calls from the worker thread are refused, not self-joined",
          "[lifecycle][callbacks]") {
  // Re-entering flush()/shutdown() from a sink (which runs on the worker
  // thread) must return an error rather than deadlocking on a self-join.
  pt_test::ScopedDb db(pt_test::unique_db_path());
  std::atomic<int> shutdown_code{-1};
  std::atomic<int> flush_code{-1};
  auto client_slot = std::make_shared<std::atomic<Client*>>(nullptr);

  auto reentrant_sink = std::make_shared<pt_test::HookSink>(
      [client_slot, &shutdown_code,
       &flush_code](const playertrace::EventBatch&) {
        Client* c = client_slot->load();
        if (c != nullptr) {
          shutdown_code.store(static_cast<int>(c->shutdown(1s).code()));
          flush_code.store(static_cast<int>(c->flush(1s).code()));
        }
      });

  auto created = Client::create(base_config(db.path, reentrant_sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  client_slot->store(client.get());

  client->start_session("anon");
  client->track("reentrant");

  // The sink's re-entrant shutdown() closes admission from inside the worker.
  // Whether that happens before or after this call reaches its lifecycle check
  // is a genuine race, so BOTH outcomes are correct: Ok if the flush got in
  // first, AlreadyShutdown if the callback had already begun shutting down.
  // What must never happen is a deadlock or a false Ok once admission closed.
  const playertrace::Status outer = client->flush(5s);
  INFO("outer flush: " << outer.message());
  CHECK((outer.ok() || outer.code() == ErrorCode::AlreadyShutdown));

  // The point of the test: re-entrant calls from the worker thread are refused
  // rather than self-joining.
  CHECK(shutdown_code.load() == static_cast<int>(ErrorCode::Internal));
  CHECK(flush_code.load() == static_cast<int>(ErrorCode::Internal));

  client_slot->store(nullptr);  // stop re-entering during teardown
  CHECK(client->shutdown(5s).ok());
}

// ---------------------------------------------------------------------------
// Callback contracts (finding #14)
// ---------------------------------------------------------------------------

TEST_CASE("Synchronously reported errors are not repeated asynchronously",
          "[lifecycle][callbacks]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  std::atomic<int> queue_full_callbacks{0};
  // The worker is pinned inside the sink so the queue reliably reaches
  // capacity; otherwise it may drain between the two track() calls.
  auto sink = std::make_shared<pt_test::BlockingSink>();
  Config config = base_config(db.path, sink);
  config.max_queue_size = 2;
  config.batch_size = 1;
  config.error_callback = [&](const playertrace::Status& s) {
    if (s.code() == ErrorCode::QueueFull ||
        s.code() == ErrorCode::StorageFull) {
      queue_full_callbacks.fetch_add(1);
    }
  };

  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  REQUIRE(client->start_session("anon").ok());
  REQUIRE(sink->wait_until_entered(10s));
  CHECK(client->track("a").ok());
  CHECK(client->track("b").ok());
  CHECK(client->track("rejected").code() == ErrorCode::QueueFull);
  sink->release();
  REQUIRE(client->flush(10s).ok());

  // QueueFull was returned to the caller, so it must not also arrive on the
  // asynchronous error callback.
  CHECK(queue_full_callbacks.load() == 0);
}

TEST_CASE("The error callback runs on the worker thread",
          "[lifecycle][callbacks]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  std::atomic<bool> saw_error{false};
  std::thread::id callback_thread;
  std::mutex m;

  Config config =
      base_config(db.path, std::make_shared<pt_test::FailingSink>());
  config.error_callback = [&](const playertrace::Status&) {
    std::lock_guard<std::mutex> lock(m);
    callback_thread = std::this_thread::get_id();
    saw_error.store(true);
  };

  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  client->start_session("anon");
  client->track("evt");
  REQUIRE(pt_test::flush_persisted(*client, 5s));

  REQUIRE(saw_error.load());
  std::lock_guard<std::mutex> lock(m);
  CHECK(callback_thread != std::this_thread::get_id());
}

TEST_CASE("A throwing error callback does not disturb the pipeline",
          "[lifecycle][callbacks]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  Config config =
      base_config(db.path, std::make_shared<pt_test::FailingSink>());
  config.error_callback = [](const playertrace::Status&) {
    throw std::runtime_error("callback exploded");
  };
  config.log_callback = [](LogLevel, const std::string&) {
    throw std::runtime_error("log exploded");
  };

  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  client->start_session("anon");
  CHECK(client->track("evt").ok());
  REQUIRE(pt_test::flush_persisted(*client, 5s));
  CHECK(client->shutdown(5s).ok());  // survives throwing callbacks
}

TEST_CASE("The log callback is actually invoked", "[lifecycle][callbacks]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  std::atomic<int> log_lines{0};
  Config config =
      base_config(db.path, std::make_shared<pt_test::CollectingSink>());
  config.log_callback = [&](LogLevel, const std::string&) {
    log_lines.fetch_add(1);
  };

  auto created = Client::create(config);
  REQUIRE(created.ok());
  auto client = std::move(created.client);
  client->start_session("anon");
  client->track("evt");
  REQUIRE(client->flush(5s).ok());
  REQUIRE(client->shutdown(5s).ok());

  // At minimum the worker announces that it started and stopped.
  CHECK(log_lines.load() >= 2);
}

TEST_CASE("Sink::flush is invoked by a client flush", "[lifecycle][sink]") {
  pt_test::ScopedDb db(pt_test::unique_db_path());
  auto sink = std::make_shared<pt_test::CollectingSink>();
  auto created = Client::create(base_config(db.path, sink));
  REQUIRE(created.ok());
  auto client = std::move(created.client);

  client->start_session("anon");
  client->track("evt");
  REQUIRE(client->flush(5s).ok());
  CHECK(sink->flushes() >= 1);
}
