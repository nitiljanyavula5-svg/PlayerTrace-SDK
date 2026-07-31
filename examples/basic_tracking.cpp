// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Minimal example: create a client, record a couple of events, flush, and shut
// down. Mirrors the quick-start in the README.
#include <chrono>
#include <cstdint>
#include <iostream>

#include <playertrace/playertrace.hpp>

int main() {
  playertrace::Config config;
  config.app_id = "forest-adventure";
  config.storage_path = "./playertrace-basic.db";
  config.consent = playertrace::ConsentState::Granted;
  config.log_callback = [](playertrace::LogLevel level,
                           const std::string& msg) {
    std::cout << "[" << playertrace::to_string(level) << "] " << msg << "\n";
  };
  config.error_callback = [](const playertrace::Status& status) {
    std::cerr << "[error] " << playertrace::to_string(status.code()) << ": "
              << status.message() << "\n";
  };

  auto result = playertrace::Client::create(config);
  if (!result.ok()) {
    std::cerr << "failed to create client: " << result.status.message() << "\n";
    return 1;
  }
  auto client = std::move(result.client);

  client->start_session("anonymous-player-42");

  client->track("level_started", {
                                     {"level_id", "forest_01"},
                                     {"difficulty", "hard"},
                                 });

  client->track("level_completed", {
                                       {"level_id", "forest_01"},
                                       {"completion_seconds", 183.7},
                                       {"deaths", std::int64_t{3}},
                                   });

  client->end_session();

  // Force everything queued to be persisted and delivered before we exit.
  playertrace::Status flushed = client->flush(std::chrono::seconds(2));
  if (!flushed.ok()) {
    std::cerr << "flush: " << flushed.message() << "\n";
  }

  client->shutdown(std::chrono::seconds(2));
  std::cout << "PlayerTrace " << playertrace::version()
            << " basic example complete. Events written to "
            << "forest-adventure-events.ndjson\n";
  return 0;
}
