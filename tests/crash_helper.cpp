// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// A helper process for genuine crash-recovery testing. The old suite described
// an orderly flush + shutdown as "surviving a restart"; this program instead
// terminates abruptly via std::_Exit(), running no destructors and performing
// no shutdown, which is what a real crash looks like to the SDK.
//
// Usage: crash_helper <db-path> <mode>
//   persist-then-crash : persist events, then die without shutdown
//   accept-then-crash  : accept events WITHOUT flushing, then die
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <playertrace/playertrace.hpp>

namespace {

/// Always fails, so nothing is ever acknowledged or removed from storage.
class NeverDeliversSink : public playertrace::Sink {
 public:
  playertrace::Status write(const playertrace::EventBatch&) override {
    return playertrace::Status(playertrace::ErrorCode::SinkError,
                               "helper: delivery disabled");
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: crash_helper <db-path> <mode>\n");
    return 2;
  }
  const std::string db_path = argv[1];
  const std::string mode = argv[2];

  playertrace::Config config;
  config.app_id = "crash-helper";
  config.storage_path = db_path;
  config.consent = playertrace::ConsentState::Granted;
  config.sink = std::make_shared<NeverDeliversSink>();
  config.batch_interval = std::chrono::minutes(10);

  auto created = playertrace::Client::create(config);
  if (!created.ok()) {
    std::fprintf(stderr, "helper: create failed: %s\n",
                 created.status.message().c_str());
    return 3;
  }
  auto client = std::move(created.client);

  if (!client->start_session("anon").ok()) {
    return 4;
  }
  client->track("alpha", {{"index", std::int64_t{1}}});
  client->track("beta", {{"index", std::int64_t{2}}});
  client->track("gamma", {{"index", std::int64_t{3}}});

  if (mode == "persist-then-crash") {
    // Force the events into durable storage, then die hard.
    const auto flushed = client->flush(std::chrono::seconds(10));
    if (!flushed.ok() && flushed.code() != playertrace::ErrorCode::SinkError) {
      std::fprintf(stderr, "helper: flush failed: %s\n",
                   flushed.message().c_str());
      return 5;
    }
  } else if (mode != "accept-then-crash") {
    std::fprintf(stderr, "helper: unknown mode '%s'\n", mode.c_str());
    return 2;
  }

  std::fflush(nullptr);
  // Abrupt termination: no destructors, no shutdown, no flush-on-exit. The
  // Client and its worker thread are simply gone.
  std::_Exit(42);
}
