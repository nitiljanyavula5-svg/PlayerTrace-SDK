# PlayerTrace

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![License](https://img.shields.io/badge/license-Apache--2.0-green.svg)
[![CI](https://github.com/nitiljanyavula5-svg/PlayerTrace-SDK/actions/workflows/ci.yml/badge.svg)](https://github.com/nitiljanyavula5-svg/PlayerTrace-SDK/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/nitiljanyavula5-svg/PlayerTrace-SDK?label=release)](https://github.com/nitiljanyavula5-svg/PlayerTrace-SDK/releases/latest)

> **v0.1.0 is released.** CI is green on Ubuntu, macOS and Windows (Debug and
> Release) with warnings-as-errors, including MSVC `/W4 /WX`, plus
> AddressSanitizer/UndefinedBehaviorSanitizer, ThreadSanitizer, `clang-format`
> and `clang-tidy`. See [Release status](#release-status).

**PlayerTrace is an engine-agnostic C++ telemetry SDK that helps game developers capture structured gameplay events without tying their projects to a specific engine or analytics provider.** It provides asynchronous tracking, offline SQLite persistence, batch processing, consent controls, and replaceable event sinks.

It is a small, dependency-light C++17 library you link into any engine or custom codebase. Events are recorded on the game thread without blocking, persisted durably to local SQLite so they survive crashes and offline play, batched, and handed to a replaceable **sink** (v0.1 ships a newline-delimited-JSON file sink). The SDK collects **no** personal or device data automatically and honors a runtime consent flag.

---

## Quick start

```cpp
#include <playertrace/playertrace.hpp>

int main() {
    playertrace::Config config;
    config.app_id = "forest-adventure";
    config.storage_path = "./playertrace.db";
    config.consent = playertrace::ConsentState::Granted;

    auto result = playertrace::Client::create(config);
    if (!result.ok()) {
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
    client->flush(std::chrono::seconds(2));   // persist + deliver at a checkpoint
    client->shutdown(std::chrono::seconds(2));
}
```

The default sink writes newline-delimited JSON to `<app_id>-events.ndjson`; supply your own `Config::sink` to send events elsewhere.

## Event pipeline

```text
 game thread                         background worker (single thread)
 -----------                         --------------------------------
 track(name, props)
   ├─ consent == Granted?  ── no ──▶ ConsentDenied (event never created)
   ├─ validate + filter
   ├─ assign session_id + seq
   └─ push to bounded queue ──full─▶ QueueFull (newest rejected)
         │  (returns Ok == Queued/Accepted)
         ▼
                                     drain a batch
                                     enforce consent + storage cap
                                     INSERT into SQLite  ◀── durable point
                                     Sink::write(batch)
                                     on success: DELETE (acknowledged)
                                     on failure: retain + retry
```

## Reliability guarantee

- `track()` returning `Ok` means the event was **accepted into the in-memory queue** (Queued) — not yet persisted. There is a documented crash-loss window between acceptance and the SQLite commit; call `flush()` at checkpoints to close it.
- Acceptance is a single linearized decision, so back-pressure is reported **synchronously**: you get `QueueFull` or `StorageFull` from `track()` rather than discovering later that the event was dropped.
- Once an event is committed to SQLite (the **durable point**), delivery is **at-least-once**: it stays in the store until a sink acknowledges it, surviving process restarts. This is **process-crash** durability; see the note in [docs/reliability.md](docs/reliability.md) about OS/power loss.
- `flush()` returns `Ok` only when everything accepted before it is persisted **and** acknowledged — a failing sink yields `SinkError`, never a misleading `Ok`.
- Delivery order follows a monotonic admission ordinal, not the wall clock, and per-session sequence numbers are gap-free across accepted events.
- Every event carries a unique UUID so downstream systems can **deduplicate**.
- The background worker is **never abandoned**. `shutdown(timeout)` returns `Ok` only once it has genuinely exited; on timeout it stays owned and running. Implement `Sink::request_cancel()` to keep shutdown bounded with a custom sink.
- One malformed event, one throwing sink, one throwing callback, or one corrupted storage row never crashes the game.

See [docs/reliability.md](docs/reliability.md) for the precise guarantees and their limits.

## Privacy defaults

- Consent defaults to `Unknown`; **nothing is collected** until you set `ConsentState::Granted`. Sessions cannot even be opened while consent is withheld.
- **No** email, real name, IP, hardware ID, advertising ID, or device fingerprint is ever collected automatically. Player identifiers are anonymous and supplied by you.
- Event and session identifiers are random UUIDs — never derived from player, device, or hardware information.
- `set_consent()` returns a `Status` and revocation is **fail-closed**: it ends the session, discards queued events, and durably purges pending ones before returning. If the purge cannot commit you get `StorageError`, collection stays blocked, and the purge is retried — including after a restart.
- A `property_filter` callback lets you strip keys before they are recorded. Validation runs on the original input, so a filter cannot mask invalid data.
- Custom sinks may send data anywhere, so PlayerTrace cannot promise data never leaves the machine — only the built-in `FileSink` is local. See [docs/privacy.md](docs/privacy.md).

See [docs/privacy.md](docs/privacy.md). Note: hashing personal data does **not** make it anonymous.

## Simulated-game output

`examples/simulated_game.cpp` plays a session, then demonstrates that accepted events survive a restart:

```text
PlayerTrace 0.1.0 simulated game
-----------------------------------------
Run 1: played a session; the output was unavailable so events
       were persisted to SQLite instead of delivered.
       (sink write failures observed: N)
Run 2: restarted, reopened the same database, and delivered
       8 event(s) that survived the restart (flush: Ok).
       Written to ./simulated_game-events.ndjson
```

Each line of the output file is one JSON object:

```json
{"event_id":"…","app_id":"simulated-game","name":"level_completed","schema_version":1,"timestamp_ms":1784868901537,"timestamp":"2026-07-24T04:55:01.537Z","session_id":"…","player_id":"anonymous-player-42","seq":4,"properties":{"level_id":"forest_01","completion_seconds":183.7,"deaths":1,"stars":2}}
```

See [docs/event-schema.md](docs/event-schema.md) for the full schema.

## Building

PlayerTrace uses CMake and vendors its dependencies (SQLite, nlohmann/json, Catch2) under `third_party/`, so a clean checkout builds with no network access or package manager.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run the tests:

```bash
ctest --test-dir build --output-on-failure
```

CMake presets are also provided (`debug`, `release`, `asan`):

```bash
cmake --preset release && cmake --build --preset release && ctest --preset release
```

### Using it in your project

After `cmake --install build`, consume it with:

```cmake
find_package(playertrace CONFIG REQUIRED)
target_link_libraries(your_game PRIVATE playertrace::playertrace)
```

Or add the repository as a subdirectory:

```cmake
add_subdirectory(third_party/playertrace)
target_link_libraries(your_game PRIVATE playertrace::playertrace)
```

## Release status

**v0.1.0 released 2026-07-31** —
[release notes and source archive](https://github.com/nitiljanyavula5-svg/PlayerTrace-SDK/releases/tag/v0.1.0).
Independent audits were performed and every confirmed finding was remediated.
All 11 hosted CI jobs are green on the released commit: Ubuntu, macOS and
Windows in Debug and Release with warnings-as-errors (MSVC at `/W4 /WX`),
AddressSanitizer + UndefinedBehaviorSanitizer, ThreadSanitizer, a build against
a system `nlohmann_json` package, `clang-format` and `clang-tidy`.

One known gap, carried forward deliberately: no vcpkg `builtin-baseline` is
pinned. This is non-blocking — the default build vendors every dependency, so a
baseline is needed only to consume the optional `system-json` feature from a
manifest build. Tracked as
[issue #2](https://github.com/nitiljanyavula5-svg/PlayerTrace-SDK/issues/2).

See [docs/release-checklist.md](docs/release-checklist.md).

## Features (v0.1)

- Custom event tracking with typed properties (`bool`, `int64_t`, `double`, `std::string`)
- Standard session events and per-session sequence numbers
- Anonymous, developer-supplied player identifiers
- Event validation (names, keys, sizes, reserved keys, duplicates)
- Thread-safe, non-blocking asynchronous queue with a single background worker
- Durable SQLite offline storage with at-least-once delivery and crash recovery
- Batch processing with a configurable size and interval
- Bounded in-memory queue **and** bounded durable storage
- Runtime consent controls with revocation semantics
- Replaceable, mockable sinks (NDJSON `FileSink` included)
- Error and logging callbacks; no exceptions across the public API
- Cross-platform (Windows, Linux, macOS) with CI and sanitizers

## Roadmap

- **v0.2** — HTTP batch sink, retry with backoff+jitter, rate limiting, delivery metrics, dead-letter handling, configurable property filtering, benchmarks.
- **v0.3** — Unreal Engine adapter, Unity native-plugin example, Python demonstration collector, local analytics dashboard, retention/funnel/progression examples.

## Documentation

- [Architecture](docs/architecture.md)
- [Reliability guarantees](docs/reliability.md)
- [Privacy model](docs/privacy.md)
- [Event schema](docs/event-schema.md)
- [Design RFC](docs/RFC-v0.1.md)

## License

Apache-2.0. See [LICENSE](LICENSE).
