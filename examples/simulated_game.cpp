// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// A small command-line "game" that exercises the SDK end to end and then
// demonstrates that persisted events survive an application restart.
//
// Run 1 ("first launch") uses a deliberately failing sink, so events are
// persisted to SQLite but never delivered — as if the network/output were
// unavailable and the app then closed normally. Run 2 ("restart") opens the
// same database with a working FileSink and flushes, delivering the events that
// survived. This shows the durability guarantee in action.
//
// Scope: this demonstrates an ORDERLY restart. Recovery from an abrupt crash
// (no destructors, no shutdown) is exercised separately by the test suite.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include <playertrace/playertrace.hpp>

namespace {

// A sink that always fails, used to simulate an unavailable output on first
// run.
class FailingSink : public playertrace::Sink {
 public:
  playertrace::Status write(const playertrace::EventBatch&) override {
    return playertrace::Status(playertrace::ErrorCode::SinkError,
                               "simulated unavailable output");
  }
};

const char* kDbPath = "./simulated_game.db";
const char* kOutPath = "./simulated_game-events.ndjson";

void remove_if_exists(const char* path) {
  std::remove(path);
}

std::size_t count_lines(const std::string& path) {
  std::ifstream in(path);
  std::size_t n = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) {
      ++n;
    }
  }
  return n;
}

// Plays one short session, emitting the standard and custom event types.
void play_session(playertrace::Client& client) {
  client.start_session("anonymous-player-42");  // -> session_start

  client.track("level_started", {
                                    {"level_id", "forest_01"},
                                    {"difficulty", "hard"},
                                });

  client.track("player_death", {
                                   {"level_id", "forest_01"},
                                   {"cause", "fell_off_ledge"},
                                   {"x", 128.5},
                                   {"y", 42.0},
                               });

  client.track("currency_transaction", {
                                           {"currency", "gold"},
                                           {"delta", std::int64_t{-50}},
                                           {"reason", "revive"},
                                           {"balance", std::int64_t{120}},
                                       });

  client.track("level_completed", {
                                      {"level_id", "forest_01"},
                                      {"completion_seconds", 183.7},
                                      {"deaths", std::int64_t{1}},
                                      {"stars", std::int64_t{2}},
                                  });

  client.track("achievement_unlocked",
               {
                   {"achievement_id", "first_forest_clear"},
                   {"hidden", false},
               });

  // A fully custom event with developer-defined properties.
  client.track("photo_mode_used", {
                                      {"filter", "sepia"},
                                      {"shots_taken", std::int64_t{4}},
                                  });

  client.end_session();  // -> session_end (with session_seconds)
}

}  // namespace

int main() {
  using playertrace::Client;
  using playertrace::Config;
  using playertrace::ConsentState;

  remove_if_exists(kDbPath);
  remove_if_exists(kOutPath);
  remove_if_exists("./simulated_game.db-wal");
  remove_if_exists("./simulated_game.db-shm");

  std::cout << "PlayerTrace " << playertrace::version() << " simulated game\n"
            << "-----------------------------------------\n";

  // ---- Run 1: first launch with the output unavailable (offline) ----
  // Note this is an orderly shutdown, not a crash: it shows that undelivered
  // events survive a normal restart. Genuine crash recovery — a process killed
  // with _Exit(), running no destructors — is covered by
  // tests/restart_tests.cpp.
  std::atomic<int> sink_failures{0};
  {
    Config config;
    config.app_id = "simulated-game";
    config.storage_path = kDbPath;
    config.consent = ConsentState::Granted;
    config.sink = std::make_shared<FailingSink>();
    config.error_callback = [&](const playertrace::Status& s) {
      if (s.code() == playertrace::ErrorCode::SinkError) {
        sink_failures.fetch_add(1);
      }
    };

    auto created = Client::create(config);
    if (!created.ok()) {
      std::cerr << "run 1: create failed: " << created.status.message() << "\n";
      return 1;
    }
    auto client = std::move(created.client);

    play_session(*client);
    client->flush(std::chrono::seconds(2));  // persists to SQLite; sink fails
    client->shutdown(std::chrono::seconds(2));

    std::cout
        << "Run 1: played a session; the output was unavailable so events\n"
        << "       were persisted to SQLite instead of delivered.\n"
        << "       (sink write failures observed: " << sink_failures.load()
        << ")\n";
  }  // client destroyed here == application exit

  // ---- Run 2: restart with a working file sink; deliver what survived ----
  {
    Config config;
    config.app_id = "simulated-game";
    config.storage_path = kDbPath;  // same database as run 1
    config.consent = ConsentState::Granted;
    config.sink = std::make_shared<playertrace::FileSink>(kOutPath);

    auto created = Client::create(config);
    if (!created.ok()) {
      std::cerr << "run 2: create failed: " << created.status.message() << "\n";
      return 1;
    }
    auto client = std::move(created.client);

    // No new session needed: the events from run 1 are already durably stored.
    playertrace::Status flushed = client->flush(std::chrono::seconds(3));
    client->shutdown(std::chrono::seconds(2));

    const std::size_t delivered = count_lines(kOutPath);
    std::cout << "Run 2: restarted, reopened the same database, and delivered\n"
              << "       " << delivered << " event(s) that survived the restart"
              << " (flush: " << playertrace::to_string(flushed.code()) << ").\n"
              << "       Written to " << kOutPath << "\n";

    if (delivered == 0) {
      std::cerr << "unexpected: no events were recovered\n";
      return 1;
    }
  }

  std::cout << "-----------------------------------------\n"
            << "Done. Inspect " << kOutPath
            << " to see the newline-delimited JSON events.\n";
  return 0;
}
