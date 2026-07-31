// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Verifies that an installed PlayerTrace can be consumed via find_package and
// exercised through its public API only (no internal headers).
#include <chrono>
#include <cstdint>
#include <iostream>

#include <playertrace/playertrace.hpp>

int main() {
  playertrace::Config config;
  config.app_id = "consumer-test";
  config.storage_path = ":memory:";  // fine for a smoke test
  config.consent = playertrace::ConsentState::Granted;

  auto created = playertrace::Client::create(config);
  if (!created.ok()) {
    std::cerr << "create failed: " << created.status.message() << "\n";
    return 1;
  }
  auto client = std::move(created.client);

  client->start_session("anon");
  playertrace::Status s =
      client->track("smoke_test", {
                                      {"ok", true},
                                      {"value", std::int64_t{42}},
                                  });
  if (!s.ok()) {
    std::cerr << "track failed: " << s.message() << "\n";
    return 1;
  }
  client->end_session();
  client->flush(std::chrono::seconds(2));
  client->shutdown(std::chrono::seconds(2));

  std::cout << "consumer-test OK against PlayerTrace " << playertrace::version()
            << "\n";
  return 0;
}
