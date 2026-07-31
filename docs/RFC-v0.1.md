# RFC: PlayerTrace SDK v0.1

**Status:** Approved (rev. 4) — approved by the project owner on 2026-07-31,
following green CI on all 11 jobs for commit eacc393.

> ⚠️ **This RFC is a historical design document and is NOT authoritative.**
> Two independent audits changed several designs described below. Where this
> file disagrees with [architecture.md](architecture.md),
> [reliability.md](reliability.md), [privacy.md](privacy.md), or
> [event-schema.md](event-schema.md), **those files are correct**. The most
> significant divergences from the original text:
>
> - The store schema is **v3**, not v1 — with `event_id NOT NULL`, an admission
>   `ordinal` column, and a durable consent-revocation marker.
> - `set_consent()` returns `Status` and revocation is **fail-closed**.
> - Capacity is enforced **at admission**, not by the worker.
> - `flush()` uses an admission-ordered **barrier**, not a batch-count guard.
> - The worker is **never detached**; `shutdown()` timing out leaves it owned
>   and running, and `Sink::request_cancel()` is the cooperative bound.
> - Storage and output paths are **exclusively owned** per process.
**Author:** Architecture
**Scope:** v0.1 (local pipeline). HTTP upload, Unreal, dashboards are explicitly out of scope.

> **Revision 3 (post-audit remediation).** An independent audit of the v0.1.0
> release candidate found defects whose fixes changed several designs described
> below. Where this document and the implementation-facing docs disagree, the
> latter are authoritative:
>
> - Admission is now **one linearized step** (lifecycle + consent generation +
>   session + capacity + sequence + enqueue) under a single lock. See
>   [architecture.md](architecture.md).
> - `max_pending_events` is enforced **at admission**, so `StorageFull` is
>   returned synchronously to the caller rather than dropped later by the worker.
> - Delivery order is a monotonic **admission ordinal**, not `created_ms`.
>   A sequence number is consumed only when admission succeeds, so accepted
>   events are gap-free.
> - Consent revocation **synchronously purges** durable storage and advances a
>   consent generation; an immediate re-grant cannot revive old work.
> - `shutdown(timeout)` is bounded even against a wedged sink.
> - Storage-write failures retain and retry the batch; `flush()` reports
>   `StorageError`.
> - The store schema is version `2` and only that exact version is accepted.
>
> This RFC remains **unapproved**, and the release checklist remains
> unfinished, until native CI and the real release process have run.

> **Revision 2 changelog** (see §15 for detail): (1) `track()` success now precisely means *Accepted/Queued*, not persisted; crash-loss window named. (2) `:memory:` SQLite is test-only. (3) Added a bounded persistent-storage cap (`max_pending_events`) with a reject-newest policy and a rejection counter. (4) Consent default is `Unknown`; revocation now discards queued + purges pending events and stops the worker, with the race boundary documented. (5) Sink payload generalized to `SerializedEvent`/`EventBatch` (no NDJSON coupling). (6) Overflow simplified to `RejectNewest` only. (7) Session-concurrency ordering rules specified. (8) Previously approved decisions preserved.

---

## 1. Product Requirements Document

### 1.1 Problem

Game developers need to understand player behavior (progression, retention, drop-off) but existing telemetry SDKs are (a) tied to a specific engine, (b) tied to a specific analytics vendor, (c) opaque about what personal data they collect, or (d) unreliable when the game runs offline. Small and mid-size studios and custom-engine teams have no lightweight, auditable option.

### 1.2 Solution

A small C++17 library that a developer links into any engine. It accepts structured gameplay events on the game thread without blocking, persists them durably to local SQLite, batches them, and hands them to a replaceable **sink** (v0.1 ships a newline-delimited-JSON file sink). It collects **no** personal or device data automatically and honors a runtime consent flag.

### 1.3 Goals (v0.1)

| # | Goal | Measure of success |
|---|------|--------------------|
| G1 | Non-blocking capture | `track()` accepts the event into the in-memory queue and returns without I/O; it never blocks on a full queue (it rejects instead) |
| G2 | Survive restarts | Events that reached SQLite (`Store::insert` committed) are delivered at-least-once after a process restart |
| G3 | Engine/vendor agnostic | Public API exposes no SQLite, nlohmann/json, or threading types |
| G4 | Privacy by default | Zero automatic PII/device collection; consent gate; property-filter hook |
| G5 | No crashes from bad input or bad sinks | Malformed event → rejected with a Status; sink throw → contained |
| G6 | Understandable API | A developer integrates basic tracking from the README in < 15 minutes |

### 1.4 Non-goals (v0.1)

HTTP upload, retry/backoff, rate limiting, crash-signal handlers, dead-letter queues, Unreal/Unity adapters, dashboards, funnels/retention analysis, property-level encryption, multi-process access to one DB file.

### 1.5 Users

- **Integrating developer** — links the SDK, calls `track()`, configures a sink. Primary audience of the public API.
- **Data engineer** (downstream) — consumes NDJSON, dedups by event ID. Audience of the event schema.

---

## 2. Public API (header-level sketch)

Namespace `playertrace`. Everything below is the *entire* public surface for v0.1.

```cpp
// result.hpp — no exceptions across the API boundary
enum class ErrorCode {
    Ok,                    // success. For track(): the event was ACCEPTED into the
                           //   in-memory queue (Queued) — NOT yet durably persisted.
    InvalidConfig, InvalidEventName, InvalidProperty,
    ReservedKey, ValueTooLarge, TooManyProperties,
    QueueFull,             // in-memory queue at max_queue_size; event rejected (RejectNewest)
    StorageFull,           // durable pending count at max_pending_events; event rejected
    ConsentDenied, NotStarted, AlreadyShutdown,
    StorageError, SinkError, Internal
};

class Status {
public:
    Status() = default;                       // Ok
    Status(ErrorCode, std::string message);
    bool ok() const noexcept;
    ErrorCode code() const noexcept;
    const std::string& message() const noexcept;
};

template <class T>
class Result {                                 // value-or-status, no exceptions
public:
    bool ok() const noexcept;
    const Status& status() const noexcept;
    T& value();                                // precondition: ok()
    // (Client uses a specialized carrier; see create())
};
```

```cpp
// consent.hpp
enum class ConsentState { Unknown, Denied, Granted };
```

```cpp
// event.hpp — typed property values, no third-party types exposed
class PropertyValue {
public:
    PropertyValue(bool);
    PropertyValue(int64_t);
    PropertyValue(double);
    PropertyValue(std::string);
    PropertyValue(const char*);                // convenience -> string
    enum class Type { Bool, Int, Double, String };
    Type type() const noexcept;
    // typed accessors, precondition-checked
};

using Properties = std::vector<std::pair<std::string, PropertyValue>>;
// NOTE: initializer-list `{{"k", v}, ...}` constructs this; insertion order preserved.
```

```cpp
// config.hpp
struct Config {
    std::string app_id;                         // required, [a-z0-9-_], <= 64
    std::string storage_path;                   // real SQLite file path (production).
                                                //   ":memory:" is supported for TESTS ONLY
                                                //   and is not a recommended production value.
    ConsentState consent = ConsentState::Unknown;   // default Unknown; NEVER defaults to Granted

    // in-memory queue / batching
    size_t   max_queue_size      = 10000;       // in-memory bound
    size_t   batch_size          = 100;
    std::chrono::milliseconds batch_interval{1000};

    // durable-storage bound (v0.1: a simple count cap, not a byte quota)
    size_t   max_pending_events  = 100000;      // conservative default; see §6 / §12
    // Overflow is RejectNewest only in v0.1 (no DropOldest — see §11).
    // In-memory-full  -> QueueFull;  durable-full -> StorageFull. Newest event is
    // rejected; already-accepted events are never silently deleted.

    // callbacks (log/error run on the worker thread; property_filter runs on the
    //   calling thread inside track(). All must be thread-safe & non-blocking.)
    std::function<void(LogLevel, const std::string&)> log_callback;
    std::function<void(const Status&)>                error_callback;
    // returns true to keep property, false to drop it (privacy filter)
    std::function<bool(const std::string& key, const PropertyValue&)> property_filter;
};
```

```cpp
// client.hpp — no singleton; instances are independent
class Client {
public:
    struct CreateResult {                       // avoids Result<unique_ptr> ergonomics
        Status status;
        std::unique_ptr<Client> client;
        bool ok() const { return status.ok(); }
    };
    static CreateResult create(Config config);

    Status start_session();                                  // anonymous
    Status start_session(std::string anonymous_player_id);   // developer-supplied
    Status end_session();

    // Non-blocking. Status::ok() means the event was ACCEPTED into the in-memory
    // queue (Queued) — it is NOT yet durably persisted. At-least-once delivery
    // begins only after the worker commits it via Store::insert. Returns QueueFull
    // / StorageFull / ConsentDenied / NotStarted / Invalid* on rejection.
    Status track(std::string event_name, Properties props = {});

    // Blocks up to timeout while the worker processes queued events through the
    // durable store and sinks. Use at checkpoints (level end, save, before exit)
    // when you want queued events persisted+delivered before continuing.
    Status flush(std::chrono::milliseconds timeout);         // drain queue+batch to sinks
    Status shutdown(std::chrono::milliseconds timeout);      // flush best-effort, persist rest

    void set_consent(ConsentState);                          // runtime-updatable
    ConsentState consent() const;

    ~Client();                                               // calls shutdown() if not already
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
};
```

```cpp
// sink.hpp — the extension point; HTTP sink in v0.2 implements this.
// Format-neutral: NO NDJSON coupling. A SerializedEvent carries the event id and
// a single JSON object as a string WITHOUT a trailing newline. Each sink decides
// how to frame a batch: FileSink appends '\n' per line; a future HTTP sink can
// join into a JSON array or wrap in a request envelope — neither reparses the other.
struct SerializedEvent {
    std::string event_id;                       // for ack / downstream dedup
    std::string json;                           // one JSON object, no trailing newline
};

struct EventBatch {
    std::vector<SerializedEvent> events;
};

class Sink {
public:
    virtual ~Sink() = default;
    // Return Ok only if the batch is durably handed off. On non-Ok, events are
    // retained in SQLite and retried later. Must not throw; if it does, the
    // worker treats it as SinkError.
    virtual Status write(const EventBatch& batch) = 0;
    virtual Status flush() { return {}; }
};

// Config gains: std::shared_ptr<Sink> sink;  (defaults to FileSink in Phase 4)
// No nlohmann/json or sqlite3 type appears in this or any other public header.
```

**Rationale highlights** (full list in §11):
- `track()` success = **Queued/Accepted**, never "persisted." Durability is the worker's job; `flush()` is the developer's tool to force it at a checkpoint.
- `CreateResult` instead of `Result<unique_ptr<Client>>` — cleaner for the one place that returns a heap object.
- `Properties` is an ordered `vector<pair>`, not `map` — preserves author order in the serialized JSON, allows duplicate-key *detection* as a validation error, and the brace-init in the spec's example just works.
- Callbacks: `log`/`error` run on the worker thread; `property_filter` runs on the calling thread. The integrator must not assume the game thread for `log`/`error`.
- Sink payload is format-neutral (`SerializedEvent`), so the FileSink's newline framing never leaks into the sink contract.

---

## 3. Component Responsibilities

| Component | File(s) | Responsibility | Depends on |
|-----------|---------|----------------|------------|
| **Client** | `client.cpp` | Public facade; validates consent/session state; enqueues; owns Worker + SessionManager. | Queue, SessionManager, Validator |
| **EventValidator** | `event_validator.cpp` | Name/key/value/size/reserved-key rules. Pure, no I/O, no locks. | — |
| **SessionManager** | `session_manager.cpp` | Session id generation, per-session monotonic sequence numbers, player id. | — |
| **EventQueue** | `event_queue.cpp` | Bounded thread-safe MPSC queue; overflow policy. | — |
| **Worker** | `worker.cpp` | Single background thread: dequeue → persist to store → batch → sink → ack/delete. | Queue, SqliteStore, Sink, Serializer |
| **SqliteStore** | `sqlite_store.cpp` | Durable pending-event store; transactional insert/fetch/ack; schema versioning; corruption handling. | sqlite3 |
| **JsonSerializer** | `json_serializer.cpp` | Event ⇄ JSON (NDJSON line). Isolates nlohmann/json. | nlohmann/json |
| **FileSink** | `file_sink.cpp` | Phase 4: NDJSON append with a real durable sync (`fsync`/`F_FULLFSYNC`/`_commit`) before returning Ok; open/write/sync failure → non-Ok with rollback to the last complete line. | — |
| **Event / PropertyValue** | `event.cpp` | Value types. | — |

Only Client, Validator, SessionManager, Queue are needed through Phase 2; Store in Phase 3; FileSink in Phase 4.

---

## 4. Event Lifecycle: `track()` → persistence → output

```
game thread                         worker thread
-----------                         -------------
track(name, props)
  ├─ consent == Granted?  ── no ──▶ return ConsentDenied (event NEVER created)
  ├─ session active?      ── no ──▶ return NotStarted
  ├─ Validator.check()    ── bad ─▶ return Invalid* (event never enqueued)
  ├─ apply property_filter (drop keys)
  ├─ build Event{ id=uuidv4, name, schema_ver, ts=utc_now,
  │              session_id, player_id?, seq=next() }   // id is random, never derived
  └─ Queue.push(event) ── full ──▶ RejectNewest: return QueueFull (event dropped)
        │ (returns Ok == QUEUED/ACCEPTED, not persisted)
        ▼
                                    loop (wake on batch_size reached OR batch_interval):
                                      batch = Queue.drain(up to batch_size)
                                      if consent != Granted: DISCARD batch, purge store, continue
                                      if Store.pending_count + batch > max_pending_events:
                                            reject the overflow tail -> StorageFull,
                                            error_callback, increment storage_rejected counter
                                      Store.insert(batch)        [TXN]  ◀─ DURABLE POINT
                                      pending = Store.fetch_pending(batch_size)
                                      if consent != Granted: purge store, continue  // don't upload
                                      status = Sink.write(serialize(pending))
                                      if status.ok(): Store.ack(pending.ids)  [TXN: delete]
                                      else:           leave in Store; error_callback; retry next loop
```

**Two distinct guarantees, stated precisely:**
- **Acceptance (Queued):** `track()` returning `Ok` means the event is in the bounded in-memory queue. Nothing about durability is promised yet.
- **At-least-once delivery** begins only at the **durable point** — when `Store::insert` commits. Between acceptance and that commit there is a **documented crash-loss window**: a process crash there loses the in-flight, not-yet-persisted events. `flush()` closes this window on demand for checkpoints. After the durable point, an event is redelivered until a sink acks it, then deleted.

---

## 5. Threading Model

- **One** background worker thread per Client. No thread pool.
- `track()` runs on the caller's thread(s); the queue is the only shared mutable state it touches (mutex + condvar, or a lock-based bounded ring).
- Multiple game threads may call `track()` concurrently (MPSC). `start_session`/`end_session`/`set_consent`/`flush`/`shutdown` are serialized by a Client-level mutex; they are expected to be called from game/control threads, not per-frame hot paths.
- Sequence numbers: generated under the session lock at enqueue time so ordering is well-defined per session.

### 5.1 Session concurrency rules (track / start_session / end_session)

A single Client-level **session lock** guards the current-session tuple `{session_id, player_id, next_seq, active}`. Both `track()` (briefly) and the session transitions acquire it, which defines a total order over these operations:

1. **Sequence assignment.** `track()` takes the session lock, reads the *current* `session_id`, assigns `seq = next_seq++`, builds the event, releases the lock, then pushes to the queue. Because the id and seq are read/assigned atomically under the same lock that `start/end_session` use, every event is unambiguously stamped with exactly one session and a gap-free, monotonic per-session sequence.
2. **Race with `end_session()`.** The lock creates a strict serialization. If a `track()` call acquires the lock **before** `end_session()`, the event is stamped with the ending session and is a valid member of it (it may be enqueued after the synthetic `session_end` event is enqueued only if it won the lock first — ordering is by lock acquisition, and the `session_end` event is itself assigned a seq under the same lock, so timeline order is consistent). If `track()` acquires the lock **after** `end_session()` has cleared `active`, it is rejected with `NotStarted`. There is no "belongs to a half-ended session" ambiguity.
3. **New session before previous end is *processed*.** Allowed. Session *transitions* are ordered by the lock, not by the worker. `end_session()` only enqueues a `session_end` event and clears `active`; `start_session()` may then set a new `session_id` and reset `next_seq` immediately, even though the previous session's events are still draining through the queue/store. Events keep their own stamped `session_id`, so interleaving in the pipeline never corrupts per-session ordering. Sessions are distinguished downstream by `session_id`, and within a session by `seq`.
4. **Determinism.** All ambiguity reduces to "who acquired the session lock first." The lock — not wall-clock time, not queue position — is the single source of ordering truth for session membership and sequence numbers.
- Callbacks (`log`, `error`, `property_filter`): `property_filter` runs on the **calling** thread (during `track`); `log`/`error` run on the **worker** thread. Documented; must be reentrant-safe and quick.
- Shutdown: sets a stop flag, wakes the worker, joins with timeout. Anything undrained stays in SQLite.

**Why single worker:** ordering is trivial, SQLite writes serialize anyway (one writer), and it removes a whole class of races. If throughput becomes a problem we revisit — but we do not optimize on assumption.

---

## 6. SQLite Schema

```sql
PRAGMA journal_mode = WAL;      -- durability + concurrent read; single writer (our worker)
PRAGMA synchronous  = NORMAL;   -- WAL-safe; FULL on explicit flush if configured

CREATE TABLE IF NOT EXISTS schema_meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
-- row: ('schema_version', '1')

CREATE TABLE IF NOT EXISTS events (
    event_id    TEXT PRIMARY KEY,      -- uuidv4; also the dedup key downstream
    app_id      TEXT NOT NULL,
    session_id  TEXT NOT NULL,
    player_id   TEXT,                   -- nullable, developer-supplied
    seq         INTEGER NOT NULL,       -- per-session sequence
    name        TEXT NOT NULL,
    schema_ver  INTEGER NOT NULL,
    ts_utc_ms   INTEGER NOT NULL,       -- epoch millis
    payload     TEXT NOT NULL,          -- serialized JSON of properties
    created_ms  INTEGER NOT NULL        -- insertion time, for FIFO retrieval
);

CREATE INDEX IF NOT EXISTS idx_events_created ON events(created_ms);
```

- **Insert:** batched inside one transaction. `INSERT OR IGNORE` on `event_id` makes re-insertion after an interrupted batch idempotent.
- **Fetch pending:** `SELECT ... ORDER BY ordinal LIMIT ?`. *(Superseded rev. 3:
  the original design ordered by `created_ms, seq`, which let a backwards clock
  step reorder delivery. Ordering is now the monotonic admission ordinal.)*
- **Ack:** `DELETE FROM events WHERE event_id IN (...)` in a transaction after sink success.
- **Bounded durable storage (`max_pending_events`):** before inserting a batch the worker checks `SELECT COUNT(*) FROM events` (maintained cheaply). If accepting the batch would exceed `max_pending_events`, the **newest** overflow events are **rejected** (not inserted) with `StorageFull`; the `error_callback` fires and an internal `storage_rejected_count` counter increments. **Older, already-persisted events are never deleted to make room** — this preserves session timelines and at-least-once semantics for events already accepted. This is a coarse *count* cap, deliberately not a byte-precise disk quota (that + a real dead-letter table are postponed to v0.2).
- **File does not immediately shrink on delete:** SQLite `DELETE` returns pages to the free list but does not return space to the OS; the `.db` file stays roughly at its high-water mark until `VACUUM` (or `PRAGMA auto_vacuum`). This is documented so operators understand disk-usage behavior; v0.1 does not auto-VACUUM on the hot path.
- **Corruption / malformed rows:** a row whose `payload` fails to parse is quarantined (logged + skipped, optionally moved to a `dead_rows` table in v0.2) rather than crashing the worker.
- **Consent purge:** on revocation the worker deletes all pending rows (`DELETE FROM events`) — see §8 / §8.1.
- **Schema mismatch:** if `schema_version` > known, refuse to open with `StorageError` (don't silently touch newer data). If lower, run forward migrations (none in v1).
- **`:memory:` is test-only:** an in-memory database is supported for deterministic unit tests but is not durable across process restart and is not a recommended production `storage_path`.

---

## 7. Error-Handling Strategy

- **No exceptions across the public API.** Internally, sqlite/json calls that can throw are wrapped and converted to `Status`. Constructors that can fail are replaced by `create()` factories.
- Three severities surfaced: return value (`Status`) for the immediate call, `error_callback` for async/worker-side failures, `log_callback` for diagnostics.
- **One bad event never crashes the game:** validation happens before enqueue; a malformed stored row is quarantined; a throwing sink is caught in the worker and converted to `SinkError` (batch retained).
- `Result<T>`/`Status` are the only error vocabulary. `ErrorCode` is stable/enumerated for programmatic handling; `message` is human-facing.

---

## 8. Privacy Model

- **Data minimization:** the SDK collects only what `track()` is given plus session/sequence/timestamp/ids it generates. No OS, device, IP, MAC, advertising id, or username collection anywhere in the codebase (enforceable by grep + a test that asserts the emitted schema).
- **Anonymous identity:** player id is optional and supplied by the developer. The SDK never derives an id from hardware or personal data. Session id is a random UUID, not derived from anything.
- **Consent default:** `ConsentState::Unknown`. It **never** defaults to `Granted`. Collection requires an explicit `Granted`.
- **Consent gate:** when not `Granted`, `track()` returns `ConsentDenied` and the event is **never created** (not collected-then-deleted).
- **Revocation is destructive by default (see §8.1):** transitioning to `Denied` (or `Unknown`) rejects new events immediately, **discards** everything still in the in-memory queue, and **purges** pending unsent rows from SQLite. The worker will not hand a batch to a sink once consent is no longer `Granted`.
- **Event identifiers are never derived** from player, device, or hardware identifiers. `event_id` and `session_id` are random UUIDs.
- **Property filter:** developer-supplied callback can strip keys (e.g. drop anything matching a PII pattern) before enqueue.
- **Docs will state plainly:** hashing an email/username does **not** make it anonymous (linkable, reversible via dictionary); do not put PII in properties. No API keys/secrets are hardcoded (there is no network in v0.1).

### 8.1 Consent-revocation sequence and race handling

`set_consent(Denied)` performs, under the Client mutex:

```
set_consent(Denied):
  1. atomically store consent = Denied         // subsequent track() -> ConsentDenied at once
  2. drain & discard the in-memory queue        // queued-but-unpersisted events dropped
  3. signal the worker to purge                  // worker DELETEs all pending SQLite rows
```

Ordering and race resolution across `track()`, the queue, SQLite, and an in-flight sink write:

| Race | Resolution |
|------|------------|
| `track()` vs. `set_consent(Denied)` | Both take the Client/consent guard. If `track()` reads `Granted` first, the event may enter the queue but is then discarded by step 2 or purged by step 3, so it is never delivered. If `set_consent` wins, `track()` sees `Denied` and returns `ConsentDenied`. |
| Events sitting in the in-memory queue | Discarded by step 2 before they can be persisted. |
| Events already committed to SQLite (not yet sent) | Purged by step 3 (`DELETE FROM events`). |
| Worker about to start a sink write | The worker re-checks consent immediately before `Sink::write`; if not `Granted` it purges and does not call the sink. |
| **A sink write already in progress** | **Honest boundary:** the SDK cannot recall bytes a sink has already begun handing off. At most **one in-flight batch** may complete after `set_consent(Denied)` returns. In v0.1 (local FileSink) that batch is only written to the local file; nothing leaves the machine. This bounded, documented gap is the single point where revocation is not instantaneous. Everything not yet in an active `Sink::write` call is stopped. |

The worker checks consent at two points — right after draining the queue and again right before `Sink::write` — so the window in which a batch can still reach a sink is reduced to exactly one already-started write.

---

## 9. Testing Strategy

Catch2. Deterministic — **no `sleep`-based timing assertions.** Techniques:
- **Injectable clock** and **injectable sink** (test doubles) via `Config`/internal seams.
- Worker exposes an internal **`drain_now()`/step** hook (test-only, not public) so batching is driven explicitly instead of by wall-clock waits.
- Concurrency tests use `N` threads pushing `M` events, then join and assert totals/sequence-uniqueness — join is the synchronization, not sleep.
- Restart recovery: open store, insert, destroy Client without acking (simulate crash), reopen, assert events still pending and get delivered.
- Consent denial/revocation, queue overflow (both policies), flush/shutdown idempotency (call twice), corrupted-row quarantine, sink-failure retention, serialization round-trips.

Coverage maps 1:1 to the spec's Testing list in §7 of the brief.

---

## 10. Repository Structure

As given in the brief (§2). No additions in v0.1 except `docs/RFC-v0.1.md` (this file) and standard `.clang-format`, `.clang-tidy`, `CONTRIBUTING.md`, `SECURITY.md`, `CHANGELOG.md` in Phase 5.

---

## 11. Major Design Decisions & Alternatives

| Decision | Choice | Alternative rejected | Why |
|----------|--------|----------------------|-----|
| Error model | `Status`/`Result`, no exceptions | Exceptions | Game engines often build `-fno-exceptions`; predictable, ABI-friendlier |
| Client creation | `create()` factory returning `CreateResult` | Throwing constructor | Fallible init without exceptions |
| Properties container | ordered `vector<pair>` | `std::map`/`unordered_map` | Preserve author order, detect dup keys, matches brace-init in spec |
| Worker | single thread | thread pool | Ordering + single SQLite writer; avoid premature optimization |
| Durability | SQLite WAL, insert-before-ack | write-ahead log of our own | Don't reinvent a proven store |
| Queue→Store timing | enqueue in memory, persist in worker | persist synchronously in `track()` | Keeps `track()` non-blocking (G1); `track()`=Queued, crash-loss window documented, `flush()` closes it |
| Delivery | at-least-once **after durable point** + dedup by event_id | exactly-once | Exactly-once across a crash boundary is not achievable cheaply; we say so |
| Sink boundary | `EventBatch` of `SerializedEvent{id, json}` (no newline) | NDJSON strings / pass `Event` objects | Keeps json/sqlite types out of the Sink ABI **and** avoids coupling every sink to FileSink's newline framing |
| Queue overflow | `RejectNewest` only | `DropOldest` | Dropping older events makes session timelines misleading; reject-newest keeps history consistent |
| Durable-storage bound | count cap `max_pending_events`, reject-newest | unbounded / byte quota / dead-letter | Bounds disk risk after permanent sink failure without half-building a v0.2 dead-letter; older events preserved |
| Consent default | `Unknown` (never `Granted`) | default `Granted` | Collect only on explicit opt-in |
| Consent revocation | discard queue + purge pending + stop worker | retain until developer flushes | Strong, predictable privacy posture; one in-flight write is the documented boundary |
| Session id | random UUID | monotonic counter / device-derived | Privacy + uniqueness across restarts |

---

## 12. Risks & Failure Cases

| Risk | Mitigation |
|------|------------|
| Crash between enqueue and Store.insert loses in-flight events | Documented as the sole loss window (`track()`=Queued, not persisted); keep batch_interval small; `flush()` closes it at checkpoints; v0.2 can add synchronous-persist option |
| Sink permanently failing → SQLite grows unbounded | **Bounded in v0.1** by `max_pending_events` (default 100k): newest events rejected with `StorageFull`, `error_callback` + `storage_rejected_count`; older persisted events preserved. Byte-precise quota + dead-letter deferred to v0.2 |
| Deleted rows don't shrink the .db file | Documented; SQLite keeps freed pages on a free list until `VACUUM`. Not auto-VACUUMed on the hot path in v0.1 |
| One in-flight sink write completes after consent revocation | Documented boundary (§8.1): at most one already-started batch; in v0.1 it only touches the local file, nothing leaves the machine |
| SQLite file on a network/removable FS with weak fsync | Document supported storage; WAL + synchronous setting; not our bug to fix silently |
| Callback throws or blocks | Wrap callbacks in try/catch on worker; document non-blocking contract |
| Clock skew / non-monotonic wall clock for `ts_utc_ms` | Timestamp is wall-clock UTC by contract (that's what analytics wants); ordering uses seq, not ts |
| Two Clients pointing at same DB file | Unsupported in v0.1; documented. Single-writer assumption |
| Very large property values | Rejected by validator (`ValueTooLarge`) with configurable cap |

---

## 13. Six-Phase Implementation Plan

| Phase | Deliverable | New deps |
|-------|-------------|----------|
| **1** | Repo scaffold, build system, value types (`Status/Result/Config/ConsentState/Event/PropertyValue`), validation, JSON serialization, Catch2 tests, basic example, README | nlohmann/json, Catch2 |
| **2** | `Client` facade, sessions + sequence numbers, bounded queue, single worker, batching, in-memory test sink, log/error callbacks | — |
| **3** | `SqliteStore`: schema versioning, transactional insert/fetch/ack, restart recovery, corruption/malformed handling | sqlite3 |
| **4** | Production `FileSink` (NDJSON), full local pipeline wired, simulated-game example with restart demo, docs (architecture/reliability/privacy/schema) | — |
| **5** | CI (Win/Linux/macOS), warnings-as-errors, ASan/UBSan on Linux, clang-format/tidy, install/package targets, consumer test, LICENSE/CONTRIBUTING/SECURITY/CHANGELOG, release checklist | — |
| **6** | (post-v0.1 hardening / bugfix buffer before tagging 0.1.0) | — |

---

## 14. Acceptance Criteria per Phase

**Phase 1** — Builds clean on the dev platform with warnings-as-errors. Validator rejects: bad names (regex), reserved keys, unsupported types, oversized values, too many properties, duplicate keys. Serialization round-trips every property type and emits **no trailing newline** in the per-event JSON (`SerializedEvent.json`). `Config::consent` defaults to `Unknown`. `ErrorCode` includes `QueueFull` and `StorageFull`. No nlohmann/json or sqlite3 type appears in any `include/` header (asserted by a header-grep test). Basic example compiles and prints a serialized event. Tests green.

**Phase 2** — `track()` is non-blocking and returns `Ok` meaning **Queued/Accepted** (proven by a test that observes the event in the queue before the worker runs; worker driven explicitly). Concurrent producers: N threads × M events all arrive with unique `(session_id, seq)` and gap-free per-session sequences. Overflow uses **`RejectNewest` only**: a full in-memory queue returns `QueueFull` and drops the newest; no older event is removed. Session concurrency (§5.1): `track()` racing after `end_session` → `NotStarted`; a new session may start before the prior session's events finish draining; sequence resets per session. `flush`/`shutdown` are idempotent (callable twice, second is a no-op Ok). No sleep-based assertions.

**Phase 3** — Events survive a simulated restart (insert, drop Client without ack, reopen, deliver). `INSERT OR IGNORE` makes an interrupted+retried batch produce no duplicates in the store; downstream dedup covered by unique event_id. **Storage cap:** with a small `max_pending_events` and a failing sink, once the cap is hit new events are rejected with `StorageFull`, `error_callback` fires, `storage_rejected_count` increments, and **no previously persisted row is deleted**. **Consent purge:** `set_consent(Denied)` empties pending rows and the worker performs no sink write afterward (beyond at most one already-started batch, per §8.1). Malformed row is quarantined, not fatal. Schema-version-too-new refuses to open. `:memory:` used only in tests. Delivery guarantee documented as **at-least-once after the durable point, not exactly-once.**

**Phase 4** — FileSink consumes `EventBatch`/`SerializedEvent` and frames each as one line by appending a newline (each line parses as one JSON object; count matches). Open/write failure → `SinkError`, batch retained in SQLite, no ack. Simulated game (real DB path, not `:memory:`) emits all required event types, demonstrates persistence across restart, and shows a `flush()` checkpoint. Four docs present and accurate to the code, including the crash-loss window, storage cap, file-shrink note, and consent-revocation boundary.

**Phase 5** — CI green on all three OSes, debug+release. Warnings-as-errors enforced. ASan/UBSan clean on Linux. `find_package(playertrace)` works in a separate consumer test project. clang-format/tidy configs present and CI-checked. Apache-2.0 headers/LICENSE in place.

---

## Overengineering call-outs / things to postpone

Flagging these now so they don't creep into v0.1:

1. **`Result<T>` as a full monadic type** — v0.1 needs only `Status` plus the one `CreateResult`. Don't build a general expected-like with map/and_then. (Postpone / drop.)
2. **Pluggable serializer interface** — only one format (NDJSON). Keep `JsonSerializer` concrete; don't add a `Serializer` abstract base until a second format exists.
3. **Multiple sink types / sink registry** — one `Sink` interface + one `FileSink` + one test sink. No registry, no composite sink in v0.1.
4. **Dead-letter queue / byte-precise disk quota** — postponed to v0.2. v0.1 ships only a coarse *count* cap (`max_pending_events`, reject-newest) — enough to bound disk risk after a permanent sink failure without building a full dead-letter subsystem.
5. **Custom lock-free queue** — a mutex+condvar bounded queue is plenty for game-thread event rates. No lock-free ring in v0.1.
6. **Schema migration framework** — v1 has one schema. A version check + refuse-if-newer is enough; no migration engine yet.
7. **Config-driven everything** — keep `Config` to the fields listed. Resist adding knobs (compression, encryption, sampling) before they're needed.

---

## Answers to the review checklist (brief §4)

- **`track()` thread-safe from multiple threads?** Yes — MPSC bounded queue, session lock for seq.
- **Queue full?** `RejectNewest` only: `track()` returns `QueueFull` and drops the newest; older events untouched. Durable-store full → `StorageFull`.
- **Persisted before "processed"?** `track()`=Queued (not persisted). `Store.insert` commit is the durable point; ack/delete only after sink success.
- **Duplicate deliveries?** At-least-once after the durable point; each event carries a UUID for downstream dedup; `INSERT OR IGNORE` prevents store dupes.
- **Broken sink crash the client?** No — worker catches throws → `SinkError`, batch retained (until the storage cap).
- **Game closes mid-batch?** Persisted-but-unacked events remain in SQLite, redelivered next run; queued-but-unpersisted events in the crash-loss window are lost unless `flush()`ed.
- **Consent revocable?** Yes — `set_consent(Denied)` blocks new events, discards the queue, purges pending SQLite, stops the worker; one already-started sink write is the documented boundary (§8.1).
- **Pre-consent events?** Never collected (default `Unknown`; not collected-then-deleted).
- **Public API expose third-party types?** No — no sqlite/json/thread types in headers (enforced by a Phase 1 header-grep test).
- **Tests deterministic?** Yes — injected clock/sink + explicit worker step; no sleeps.
- **API small enough?** ~11 public methods on one class plus a handful of value types.

---

## 15. Revision 2 — detailed changelog & tradeoffs

### Changed sections
1. **§1.3 / §2 / §4 — Acceptance vs. delivery.** `track()` `Ok` now explicitly means **Queued/Accepted**. `ErrorCode::Ok` documents the semantic; at-least-once starts at the `Store::insert` durable point; the crash-loss window is named; `flush()` is the checkpoint tool.
2. **§2 / §6 — `:memory:` is test-only.** Config comment + schema note; production examples use a real path.
3. **§2 / §6 / §12 — Bounded durable storage.** New `Config::max_pending_events` (default 100000), reject-newest with `StorageFull`, `error_callback`, `storage_rejected_count`; older events never deleted to make room; documented that deletes don't shrink the file without `VACUUM`.
4. **§2 / §8 / §8.1 — Consent.** Default `Unknown`, never `Granted`. Revocation discards queue + purges pending + stops worker; new §8.1 gives the sequence and a race-resolution table; one in-flight sink write named as the honest boundary.
5. **§2 — Sink payload generalized.** `SerializedEvent{event_id, json-without-newline}` + `EventBatch{events}`; FileSink appends newlines, HTTP sink builds its own envelope. No NDJSON coupling.
6. **§2 / §11 — Overflow simplified.** `RejectNewest` only; `DropOldest` removed (would make session timelines misleading).
7. **§5.1 — Session concurrency.** Explicit rules for seq assignment, `end_session` races, starting a new session before the old one drains, and lock-as-ordering-truth.
8. **Preserved:** C++17; ordered `vector<pair>`; `CreateResult`; no singleton; replaceable sinks; sqlite/json hidden from headers; at-least-once after durable persistence; injected clock/test doubles; no arbitrary sleeps; no HTTP/Unreal/dashboard/lock-free queue/serializer registry/migration framework in v0.1; event IDs never derived from player/device/hardware identifiers.

### Tradeoffs & contradictions introduced (called out honestly)
- **Consent purge vs. at-least-once.** Revocation deliberately *deletes* accepted, persisted-but-unsent events. This is an intentional exception to at-least-once: privacy wins over delivery when consent is withdrawn. Documented as such — the delivery guarantee is "at-least-once **while consent remains Granted**."
- **Storage cap vs. at-least-once.** When `max_pending_events` is reached, *new* events are dropped (`StorageFull`). At-least-once therefore applies to *accepted* events only; acceptance itself is best-effort under back-pressure. Reject-newest (not drop-oldest) keeps the guarantee meaningful for events already in the store.
- **Non-blocking `track()` vs. durability.** Unchanged and intentional: the crash-loss window is the price of a non-blocking hot path. `flush()` is the escape hatch; a synchronous-persist mode is a v0.2 option, not a v0.1 default.
- **Consent revocation is not perfectly instantaneous.** One already-started `Sink::write` may finish. In v0.1 that only writes to the local file, so nothing is transmitted; but the boundary is real and documented rather than papered over.
- **`storage_rejected_count` surface.** For v0.1 this counter is delivered via the `error_callback` stream (and can back a future `Metrics` accessor in v0.2); v0.1 does not add a public metrics API to avoid premature surface.
