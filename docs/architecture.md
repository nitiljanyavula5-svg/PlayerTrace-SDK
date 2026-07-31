# Architecture

PlayerTrace is a small C++17 library with a deliberately narrow public surface
(one `Client` class plus value types) and a set of internal components hidden
behind a PIMPL. No SQLite, nlohmann/json, threading, or file-stream types appear
in any public header — `Client` and `FileSink` both hide their implementation
behind an opaque pointer.

## Component overview

```text
        ┌─────────────────────────── public API (include/playertrace) ───────────────────────────┐
        │  Client   Config   Event/PropertyValue   EventBuilder   Sink/FileSink   Status/Result    │
        └───────────────────────────────────────────┬────────────────────────────────────────────┘
                                                     │ (PIMPL)
        ┌────────────────────────────────────────────▼───────────────────────────────────────────┐
        │ Client::Impl                                                                             │
        │   ├─ EventValidator      names / keys / sizes / reserved keys / duplicates (pure)        │
        │   ├─ SessionManager      session id, per-session sequence, duration (one mutex)          │
        │   ├─ IdGenerator         random UUIDv4 (no hardware/personal derivation)                 │
        │   ├─ EventQueue          bounded, thread-safe, RejectNewest overflow                     │
        │   └─ Worker (1 thread)   drain → persist → deliver → acknowledge                          │
        │        ├─ SqliteStore    transactional durable store, schema versioning                  │
        │        ├─ JsonSerializer isolates nlohmann/json                                           │
        │        └─ Sink           FileSink (NDJSON) or a developer-supplied sink                  │
        └──────────────────────────────────────────────────────────────────────────────────────────┘
```

| Component | Responsibility |
|-----------|----------------|
| **Client / Impl** | Public facade. Validates consent and session state, assigns identity + sequence, enqueues, owns the worker. |
| **EventValidator** | Pure validation. No I/O, no locks. |
| **SessionManager** | Session identity, per-session monotonic sequence, duration. |
| **IdGenerator** | Random UUIDv4 generation. |
| **EventQueue** | Bounded MPSC queue with `RejectNewest` overflow and batch-aware wakeups. |
| **Worker** | The single background thread; the only thread that delivers to the sink. |
| **PathClaim** | Process-wide registry making storage and output paths exclusively owned. |
| **SqliteStore** | Durable pending-event store: transactional insert, FIFO fetch, acknowledge/delete, purge, schema versioning, corruption quarantine. |
| **JsonSerializer** | Event → JSON string. The only user of nlohmann/json. |
| **FileSink** | NDJSON output sink. Any `Sink` implementation is a drop-in replacement. |

## Threading model

- **One** background worker thread per `Client`. No thread pool.
- The worker and the `Client` co-own a shared `Pipeline` (config, queue, store,
  sink, stats, consent) through `shared_ptr`. The worker thread never touches
  anything owned by `Client::Impl`, which keeps the two lifetimes independent
  and the locking simple. It is **not** a licence to abandon the thread — see
  "Worker lifetime" below.
- `track()` runs on the caller's thread(s). Multiple game threads may call it
  concurrently (multi-producer). It touches only the validator (stateless), the
  session manager (brief lock for sequence assignment), and the queue.
- `start_session` / `end_session` / `set_consent` / `flush` / `shutdown` are
  serialized by a client-level mutex and are meant for game/control threads, not
  per-frame hot paths.
- The SQLite handle is shared: the worker uses it for the delivery path, and
  `Client::create()` and `set_consent()` use it from a caller thread for
  recovery and for the durable consent purge. Every access is serialized by the
  pipeline's store mutex, which is never held across a call into user code.

### Worker lifetime

The worker thread is **always joined** before the `Client` is destroyed; it is
never detached, not even when `shutdown()` times out. `shared_ptr` keeps the
sink and callback *objects* alive, but not the state those callables
**capture** — an engine reference, a caller-owned file, a captured `this`.
A detached worker could invoke that user code after `~Client()` returned.

The cost is explicit: `~Client()` blocks until the worker exits, so a custom
sink that never returns will block destruction. `Sink::request_cancel()` is the
cooperative contract that keeps shutdown bounded. See
[reliability.md](reliability.md).
- Callbacks: `property_filter` runs on the **calling** thread inside `track()`;
  `log_callback` and `error_callback` run on the **worker** thread, or on the
  thread calling `Client::create()`/`set_consent()` for failures raised there.
  All must be thread-safe and non-blocking, and must not re-enter the client.
  `flush()`/`shutdown()` called from the worker thread return
  `ErrorCode::Internal` rather than dead-locking.

### Admission: one linearized decision

A single **admission lock** in `Client::Impl` guards the entire accept decision:
lifecycle state, consent state and generation, session state, durable capacity,
sequence assignment, ordinal assignment, and the queue push. Session transitions
(`start_session`, `end_session`) take the same lock and emit their markers while
holding it.

Holding one lock for all of it is what makes the following agree with each other
under concurrency:

1. **Sequence assignment.** Validation and the property filter run *before* the
   lock (so a slow filter does not serialize producers, and invalid input never
   reaches admission). Inside the lock, capacity and queue space are checked
   **before** a sequence number is taken, so a rejected event never consumes one.
   Accepted events are therefore gap-free.
2. **Race with `end_session()`.** Ordering is by lock acquisition. A `track()`
   that wins the lock first belongs to the ending session; one that arrives after
   the session was deactivated returns `NotStarted`. `session_end` is emitted
   under the same lock immediately before deactivation, so it always follows
   every event of its session.
3. **New session before the old one drains.** Allowed. A new session may start
   (new id, sequence reset to 0) while the previous session's events are still in
   the pipeline. Events carry their own `session_id`.
4. **Race with `set_consent()`.** `track()` records the consent generation on
   entry and re-checks it under the lock, so a call that spans a revocation is
   rejected rather than admitted afterwards.
5. **Race with `shutdown()`.** Shutdown moves the lifecycle to `Stopping` under
   the same lock before draining, so no producer can be told `Ok` for work the
   stopping worker will not process.

### Delivery ordering

Delivery order is the monotonic **admission ordinal**, never the wall clock:
`ORDER BY ordinal`. A clock that steps backwards, or two events in the same
millisecond, cannot reorder anything. On startup the ordinal continues from
`MAX(ordinal)` in the store so recovered events still sort ahead of new ones.

## Event lifecycle

```text
track() ─▶ validate ─▶ property_filter ─▶ assign session+seq ─▶ enqueue (Ok = Queued)
                                                                     │
worker loop:  drain batch ─▶ consent check ─▶ storage-cap check ─▶ INSERT (durable point)
              ─▶ fetch pending ─▶ consent re-check ─▶ Sink::write ─▶ on ok: DELETE (ack)
                                                                    on fail: retain + retry
```

`flush(timeout)` asks the worker to drain the queue fully and attempt delivery,
then blocks until that pass completes or the timeout elapses. `shutdown(timeout)`
performs a best-effort flush and stops the worker; anything undelivered stays in
SQLite for the next run.

## Design decisions

See [RFC-v0.1.md](RFC-v0.1.md) §11 for the full decision log and alternatives.
Highlights: exception-free `Status`/`Result` error model (engines often build
with `-fno-exceptions`); a single worker (trivial ordering, one SQLite writer);
`Properties` as an ordered vector (preserves author order, detects duplicates);
a format-neutral `SerializedEvent` sink payload (no NDJSON coupling); vendored
dependencies for a self-contained build.
