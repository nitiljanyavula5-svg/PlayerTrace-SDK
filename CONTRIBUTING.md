# Contributing to PlayerTrace

Thanks for your interest in PlayerTrace. This document describes how to build,
test, and submit changes.

## Principles

- **Small, understandable public API.** New public surface should be justified.
- **No third-party types in public headers.** SQLite and nlohmann/json stay
  behind the PIMPL and internal headers.
- **Deterministic tests.** No arbitrary `sleep` calls; drive the worker with
  `flush()` and use test doubles / injected clocks.
- **Exception-free public API.** Return `Status`; do not let exceptions cross the
  API boundary.
- **Privacy first.** Never add automatic collection of personal or hardware data,
  and never derive identifiers from such data.

## Building

The repository vendors its dependencies under `third_party/`, so a clean checkout
builds with no network access.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPLAYERTRACE_ENABLE_WERROR=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Presets are available (`debug`, `release`, `asan`):

```bash
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
```

## Code style

- C++17, Google-based style. Run `clang-format` (config in `.clang-format`):

  ```bash
  clang-format -i include/playertrace/*.hpp src/*.cpp src/internal/*.hpp
  ```

- Static analysis with `clang-tidy` (config in `.clang-tidy`). The
  `HeaderFilterRegex` restricts checks to first-party code.

- New code must compile warning-clean with `-DPLAYERTRACE_ENABLE_WERROR=ON` on
  GCC/Clang and `/W4 /WX` on MSVC.

## Tests

- Every behavior change needs a test. Test files live in `tests/` and use Catch2.
- Prefer testing internal components directly (e.g. `EventQueue`, `SqliteStore`,
  `EventValidator`) plus end-to-end coverage via `Client`.
- Use the helpers in `tests/test_support.hpp` (`CollectingSink`, `FailingSink`,
  `ThrowingSink`, `BlockingSink`, `HookSink`, `ScopedDb`, `ScopedFile`).
- To inject a deterministic clock, inject storage faults, or read internal
  counters, use `playertrace::ClientInternal` (declared in
  `src/internal/client_internal.hpp`). For FileSink I/O faults use
  `playertrace::FileSinkInternal` (`src/internal/file_sink_internal.hpp`).

### Writing deterministic concurrency tests

The background worker wakes as soon as `batch_size` events are queued, so
"fill the queue and assume it stays full" is a race, not a test. To make queue
and back-pressure behavior deterministic, park the worker inside a
`BlockingSink` first (`wait_until_entered()`), then drive the producer side;
while the worker is blocked it cannot drain. Release the sink and `flush()` to
observe the result.

Assert on **event identity and order** (names, `seq`, `event_id`), not just on
counts, and never on a `std::set` of results — that discards the ordering the
SDK actually promises.

Real crash recovery is tested with `tests/crash_helper.cpp`, a child process
that terminates via `std::_Exit()` so no destructor or shutdown path runs. An
orderly `flush()` + `shutdown()` is *not* a crash and must not be described as
one.

## Pull requests

1. Branch from `main`.
2. Keep changes focused; explain non-obvious design tradeoffs in the description.
3. Ensure CI is green (Windows, Ubuntu, macOS; debug + release; sanitizers on
   Linux).
4. Update the relevant docs (`docs/`) and `CHANGELOG.md`.

## Scope

v0.1 is intentionally local-only. HTTP upload, retries, rate limiting, engine
adapters, and dashboards are planned for later milestones — see the roadmap in
the README. Please open an issue to discuss larger features before implementing.

## License

By contributing, you agree that your contributions are licensed under the
Apache-2.0 license (see `LICENSE`).
