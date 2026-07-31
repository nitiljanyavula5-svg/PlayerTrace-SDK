# Changelog

All notable changes to PlayerTrace are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

_Nothing yet._

## [0.1.0] - 2026-07-31

Initial release: a complete local telemetry pipeline. Every audit
remediation performed against the release candidate is folded in below;
because 0.1.0 was never published, those fixes are part of this release
rather than a separate version.

### Added

- **Event model** — unique event id, name, schema version, UTC timestamp,
  session id, optional anonymous player id, per-session sequence number, and a
  flat typed property map (`bool`, `int64_t`, `double`, `std::string`).
- **Public client API** — `Client::create`, `start_session`, `end_session`,
  `track`, `flush`, `shutdown`, runtime `set_consent`, and log/error callbacks.
  No global singleton; multiple independent clients are supported.
- **Event validation** — rejects invalid names, unsupported/oversized values,
  reserved keys, duplicate keys, and too many properties.
- **Asynchronous pipeline** — a bounded, thread-safe in-memory queue with a
  `RejectNewest` overflow policy and a single background worker; configurable
  batch size and interval.
- **Durable storage** — SQLite-backed store with transactional inserts, FIFO
  retrieval, acknowledgment/deletion, schema versioning, corruption quarantine,
  and restart recovery. At-least-once delivery after the durable point; unique
  event ids for downstream deduplication.
- **Bounded durable storage** — `max_pending_events` cap with a reject-newest
  policy and a rejection counter, to bound disk usage after a permanent sink
  failure.
- **Sinks** — a format-neutral `Sink` interface and a production `FileSink` that
  writes newline-delimited JSON. Sinks are replaceable and mockable.
- **Privacy controls** — consent defaults to `Unknown`; runtime revocation
  discards queued and purges pending events; a `property_filter` callback; random
  identifiers never derived from personal or hardware data.
- **Error handling** — exception-free public API built on `Status`, `Result`, and
  `ErrorCode`; one malformed event or throwing sink never crashes the game.
- **Tests** — Catch2 unit and integration tests (validation, serialization
  round-trips, sessions and sequence numbers, concurrent producers, bounded
  queue, storage restart/duplicate/malformed/schema cases, consent
  denial/revocation, flush/shutdown idempotency, sink failures). Deterministic;
  no arbitrary sleeps.
- **Examples** — `basic_tracking` and a `simulated_game` that demonstrates
  persistence across a simulated restart.
- **Build & packaging** — CMake build with vendored dependencies (SQLite,
  nlohmann/json, Catch2), CMake presets, a vcpkg manifest, install/package
  targets, and a `find_package` consumer test.
- **CI** — GitHub Actions on Windows, Ubuntu, and macOS; debug and release;
  warnings-as-errors; AddressSanitizer + UndefinedBehaviorSanitizer on Linux.
- **Documentation** — architecture, reliability, privacy, and event-schema docs,
  plus the design RFC.

### Fixed — pre-publication review

A further independent review of the prepared repository found four defects that
had to be resolved before publication. Each has a regression test that fails
against the unfixed tree.

- **Expired flush-token results.** Completion history is bounded, and the
  nearest-match lookup used to read it slid forward onto a NEWER token's entry
  once an old record was pruned — reporting an unrelated flush's success as if
  it belonged to the expired one. Each token now records its own entry, lookup
  is by exact key, and a token whose record has been discarded reports that
  explicitly instead of borrowing another flush's status. A token the worker
  never serviced because it had already stopped is reported distinctly from one
  that expired; conflating the two made clean shutdowns look like failures.
- **Incorrect SQLite memory-URI exemption.** `file:`-style names were treated as
  in-memory databases and exempted from `PathClaim`. This build compiles SQLite
  without `SQLITE_USE_URI` and opens with plain `sqlite3_open()`, so those
  strings are ordinary filenames — the exemption was one SQLite never honoured,
  and any path containing `mode=memory` escaped exclusive ownership, letting two
  clients share one database undetected. Only `:memory:` is exempt now, and the
  build fails if URI support is enabled without revisiting the parser.
- **Schema CHECK validation bypass.** The required
  `CHECK(length(event_id) > 0)` was matched as raw text against the stored DDL,
  so the same characters placed inside a string literal or a comment satisfied
  validation while no real constraint existed. Comments and all four SQLite
  quoting forms are now blanked before scanning; `CHECK` is matched as a whole
  token, its parenthesised expression extracted with balanced nesting, and the
  expression itself compared. `idx_events_order` is validated structurally
  through `PRAGMA index_list`/`index_info` — rejecting a partial, unique or
  auto-generated index, and one over the wrong column.
- **Strict GCC build failure.** `iso8601_utc()` passed seven unbounded `int`
  fields to `snprintf`, which GCC 13 reports as `-Wformat-truncation` under the
  documented `-Werror` build. The fields are now range-checked before
  formatting, which both answers the warning with a proof rather than a
  suppression and fixes a real defect: a calendar conversion can yield a year
  outside four digits, which would have emitted a timestamp that is not
  ISO-8601.

### Fixed — third-party re-audit merge

An independent re-audit re-derived the outstanding findings against this tree
and added deterministic coverage for four release blockers. Regression tests for
every item below fail against the unmerged tree and pass against this one.

- **Atomic session replacement.** `start_session()` on an already-active session
  saved its rollback snapshot BEFORE ending the old session, then restored that
  snapshot if the replacement `session_start` was rejected. Because the old
  session's `session_end` had already been admitted — and an admitted event
  cannot be withdrawn — this resurrected the ended session with its sequence
  counter rewound, delivering events after their own `session_end` and reusing
  `(session_id, seq)`. The transition now reserves capacity for both markers up
  front, and an ended session is never restored.
- **Terminal failure closes admission.** When the worker exited unexpectedly,
  `flush()` and `shutdown()` surfaced the terminal status but `track()`,
  `start_session()` and `end_session()` kept returning `Ok` into a queue nothing
  would ever drain. A gate shared through the pipeline now closes admission
  permanently as the worker dies. Consent revocation is unaffected and still
  purges synchronously.
- **Per-token flush outcomes.** A single most-recent-status slot meant a waiter
  descheduled past a later pass read that pass's result. Each pass now records
  its own outcome, a waiter takes the first pass covering its token, and
  retention is bounded so abandoned tokens cannot accumulate records.
- **Exact schema-v3 validation.** `schema_meta`'s shape is now validated, the
  non-empty `event_id` CHECK is verified from the stored DDL (PRAGMA
  `table_info` cannot see constraints), and `idx_events_order` must actually
  index `events(ordinal)` rather than merely exist under that name. All of it is
  read-only and runs before any mutating PRAGMA, so a refused database is left
  byte-for-byte unchanged.
- **Flush error scoping.** An error recorded before a flush was requested — and
  since resolved by a successful retry — was still charged to that flush,
  reporting failure for data that was durable. Errors are now scoped to the pass
  that observes them; anything genuinely outstanding is still caught by the
  residual check.
- **Path exclusivity and in-memory URIs.** Storage and `FileSink` output claims
  shared one namespace, so a sink could no longer claim the file backing the
  store. SQLite in-memory databases are now recognized by parsing the URI —
  `:memory:`, `file::memory:`, and `mode=memory` as a whole query parameter —
  instead of substring matching, which had let an ordinary file whose *name*
  contained `mode=memory` skip the exclusivity claim entirely.
- **64-bit file offsets on POSIX.** `_FILE_OFFSET_BITS=64` is defined for UNIX
  builds and `file_sink.cpp` static_asserts a 64-bit `off_t`, so a 32-bit POSIX
  configuration fails to build instead of silently truncating NDJSON past 2 GiB.

Verified unchanged and newly pinned by tests: the fail-closed durable revocation
protocol (failed marker, failed purge, refused re-grant, restart recovery) and
the shutdown budget (early cooperative cancellation with time reserved for
joining, no detach, unconditional destructor join).

### Fixed — second-audit remediation

A second independent audit of the repaired candidate found further defects.
All confirmed findings were remediated.

- **Consent revocation is now atomic and fail-closed.** The final consent and
  generation check happens while holding the storage lock immediately before the
  durable insert, so a batch drained before a revocation can no longer land in
  storage after the purge reported success. Revocation commits a durable marker
  first, then purges and clears it in one transaction; a failed purge keeps the
  marker, keeps collection blocked, does **not** reset capacity accounting, and
  is retried on the next consent call and on the next open. Re-granting is
  refused while a purge is owed. Opening with consent withheld purges recovered
  events before the worker starts. `set_consent()` now returns `Status`.
- **The worker is never abandoned.** A timed-out `shutdown()` no longer detaches
  the thread — `shared_ptr` kept the sink and callback objects alive but not the
  state they capture, so a detached worker could call into freed user state. The
  client keeps owning a joinable worker, `Stopped` is reached only after a real
  exit, and every caller observes that same truth. `Sink::request_cancel()` and
  `Client::wait_for_worker_exit()` were added; `~Client()` joins and may block if
  a custom sink refuses to return, which is now documented rather than hidden.
- **FileSink no longer destroys acknowledged output.** The rollback point is
  obtained by seeking to the real end of file: `ftell()` returns 0 immediately
  after `fopen(..., "ab")`, so a first-write failure after reopening truncated
  the entire file. Buffers are flushed before truncation (otherwise the removed
  bytes were written again), offsets are 64-bit, torn final lines are repaired on
  open, a rollback that cannot complete fails the sink permanently, and a path
  may be owned by only one live sink.
- **Session markers are atomic.** `start_session()`/`end_session()` commit
  session state only if the marker was admitted, so a rejected marker no longer
  produces a markerless session or a `session_end` with no `session_start`.
  Revocation ends the active session.
- **`flush()` tells the truth.** The fixed 1,024-batch guard is replaced by an
  admission-ordered barrier, and `Ok` is returned only when nothing accepted
  before the flush remains queued, retrying, persisted-unsent, or unacknowledged.
  Fetch, acknowledgment, and quarantine failures now reach the flush result.
- **Storage integrity.** Schema v3 makes `event_id NOT NULL` and non-empty — a
  NULL key could never be acknowledged and was redelivered forever. Deletions
  are verified with `sqlite3_changes()`. The schema is inspected read-only
  *before* any PRAGMA, so refusing an unsupported database leaves it
  byte-for-byte unchanged, and a database claiming v3 has its exact column and
  index shape validated. Rollback, close, and clear-bindings results are checked.
- **Accounting.** Durable capacity is reserved before the event is published to
  the queue (publishing first allowed the worker to release it before the
  increment landed, permanently leaking capacity). Failed batches are held in the
  worker's own retry buffer instead of being pushed back past the queue bound.
  Quarantined rows release their capacity exactly once, when the delete commits.
- **Exclusive paths.** Two clients can no longer share a storage path, and two
  FileSinks can no longer share an output path.
- **Documentation** was corrected where it overstated behavior: durability is
  scoped to process-crash recovery, custom sinks may transmit off-machine, and
  the RFC is explicitly marked non-authoritative.

### Fixed — first-audit remediation of the 0.1.0 release candidate

An independent audit of the release candidate identified defects across process
safety, concurrency, persistence, privacy, and packaging. All confirmed findings
were remediated before release. Because 0.1.0 has never been published, these
are folded into the initial release rather than being a separate version.

- **Process safety.** Ill-formed UTF-8 in a property value or player id was
  accepted by `track()` and later terminated the host process when the worker
  serialized it. UTF-8 is now validated at admission, serialization returns a
  `Status` instead of throwing, and the worker thread has an outermost exception
  barrier. Public API entry points translate exceptions into `Status`.
- **Consent.** Revocation now advances a consent generation and purges durable
  storage synchronously, so an in-flight `track()`, an immediate re-grant, or a
  crash cannot resurrect revoked data. Revocation also works after shutdown.
  `start_session()` is refused while consent is withheld.
- **Admission and lifecycle.** Lifecycle, consent, session, capacity, sequence
  assignment, and enqueue are now one linearized step, so `track()` can no
  longer return `Ok` for work a stopping worker will never process. Concurrent
  `shutdown()` callers wait for the real terminal state.
- **Ordering.** Delivery is ordered by a monotonic admission ordinal instead of
  wall-clock time (which could reorder events when the clock stepped backwards),
  and the ordinal continues across restarts. A rejected event no longer consumes
  a sequence number, so accepted events are gap-free. Session markers correctly
  bracket concurrently tracked events.
- **Shutdown.** `shutdown(timeout)` no longer blocks indefinitely when a sink is
  wedged; it returns `Timeout` and leaves the worker safely detached, since the
  worker co-owns all state it touches. Calling `flush()`/`shutdown()` from the
  worker thread returns an error instead of dead-locking on a self-join.
- **Persistence.** A failed insert, bind, step, or commit no longer discards
  accepted events: the batch is rolled back, retained, and retried in order, and
  `flush()` reports `StorageError`. Every SQLite return code is now checked;
  `count()` failure is never reported as an empty store; the requested journal
  mode is verified rather than assumed; and a quarantine is only reported when
  its deletion actually commits.
- **Schema safety.** The store version is inspected before any DDL, so refusing
  an unsupported database leaves it unmodified. Malformed, empty, negative, and
  overflowing version values are rejected. The schema version is now `2` and only
  that exact version is accepted.
- **Durable capacity.** `max_pending_events` is enforced at admission and
  returns `StorageFull` to the caller, instead of silently accepting events and
  discarding them later. Events recovered from a previous run count against it.
- **FileSink.** Now performs a real durable sync (`fsync`/`F_FULLFSYNC`/
  `_commit`) before reporting success, truncates back to the last complete line
  after a partial write, and reopens rather than staying poisoned after a
  failure. Its implementation moved behind a PIMPL so no stream or mutex type
  appears in a public header.
- **Callbacks.** `error_callback` is no longer invoked for conditions already
  reported synchronously by `track()`. `log_callback` is now actually invoked,
  and `Sink::flush()` is called during a client flush. The documented threading
  and reentrancy contract was corrected to match the implementation.
- **Validation.** Properties are validated before the privacy filter runs, so a
  filter cannot mask invalid input. Player-id length, non-positive
  `batch_interval`, and out-of-range capacity values are now rejected.
- **Packaging.** The install now ships `LICENSE`, `THIRD_PARTY_NOTICES.md`, and
  the vendored nlohmann/json MIT notice. `PLAYERTRACE_USE_SYSTEM_JSON=ON`
  produces a consumable package (the config emits `find_dependency`). All
  embedded SQLite symbols are renamed to `playertrace_sqlite3_*` so an
  application can link its own SQLite. PlayerTrace no longer forces a build type
  or install rules onto a parent project, and the library type is explicitly
  static for v0.1.
- **Tests and docs.** Added crash-recovery testing using a child process
  terminated with `_Exit()`, storage and FileSink fault injection, latch-driven
  concurrency tests, and packaging tests (install manifest, co-link, subproject
  isolation). Tests that claimed more than they proved were renamed or rewritten,
  and the sequence-gap contradiction between the architecture and reliability
  docs was resolved.

### Known limitations

- No HTTP upload, retries, rate limiting, or dead-letter queue (planned for v0.2).
- Deleting rows does not immediately shrink the SQLite file (no auto-VACUUM).
- A single already-started sink write may complete after consent revocation.

[Unreleased]: https://github.com/nitiljanyavula5-svg/PlayerTrace-SDK/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/nitiljanyavula5-svg/PlayerTrace-SDK/releases/tag/v0.1.0
