// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Minimal parent-project application that uses an embedded PlayerTrace.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>

#include <playertrace/playertrace.hpp>

int main() {
  playertrace::Config config;
  config.app_id = "parent-app";
  config.storage_path = ":memory:";
  config.consent = playertrace::ConsentState::Granted;

  auto created = playertrace::Client::create(config);
  if (!created.ok()) {
    std::fprintf(stderr, "create failed: %s\n",
                 created.status.message().c_str());
    return 1;
  }
  auto client = std::move(created.client);
  if (!client->start_session("anon").ok()) {
    return 1;
  }
  if (!client->track("embedded_ok", {{"n", std::int64_t{1}}}).ok()) {
    return 1;
  }
  client->end_session();
  client->flush(std::chrono::seconds(5));
  client->shutdown(std::chrono::seconds(5));
  std::printf("parent-app OK against PlayerTrace %s\n", playertrace::version());
  std::remove("./parent-app-events.ndjson");
  return 0;
}
