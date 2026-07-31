# Reliability

PlayerTrace separates two distinct guarantees. Being precise about the boundary
between them is the whole point of this document.

> **Scope of "durable".** The SQLite guarantees below are **process-crash**
> recovery: if the application dies, committed events survive. The store runs
> with `synchronous=NORMAL` in WAL mode, which does not by itself guarantee
> survival of an OS crash or sudden power loss. `FileSink` output *is* forced to
> disk with `fsync`/`F_FULLFSYNC`/`_commit` before acknowledgment.

## 1. Acceptance (Queued)

`track()` returning `Status::ok()` means the event was **accepted into the
bounded in-memory queue**. It is *not* yet durably persisted. This keeps
`track()` non-blocking on the game thread.

Acceptance is decided as a single linearized step that checks, in order:
lifecycle state, consent, an active session, durable capacity, and queue space.
Because the decision is atomic, a caller is never told `Ok` for work the SDK
cannot process:

| Condition | Returned to the caller |
|---|---|
| Client is shutting down or stopped | `AlreadyShutdown` |
| Consent is not `Granted` | `ConsentDenied` |
| No active session | `NotStarted` |
| Durable store already holds `max_pending_events` | `StorageFull` |
| In-memory queue at `max_queue_size` | `QueueFull` |

`QueueFull` and `StorageFull` reject the **newest** event; already-accepted
events are never discarded to make room. These are returned synchronously and
are deliberately *not* also reported through `error_callback`, so one problem is
never reported twice.

## 2. At-least-once delivery (after the durable point)

The background worker drains the queue and commits events to SQLite in a single
transaction. That commit is the **durable point**. From then on:

- The event remains in SQLite until a sink acknowledges it, then it is deleted.
- If a sink fails, the batch is retained and retried on a later loop.
- If the process restarts, un-acknowledged events are still in SQLite and are
  delivered on the next run.
- Because delivery is at-least-once, an event may be delivered more than once
  (for example if a crash, or a failing acknowledgment, happens after the sink
  succeeded but before the deleting transaction committed). Every event carries
  a unique `event_id` (UUIDv4) so downstream systems can **deduplicate**.

We deliberately do **not** claim exactly-once delivery. Exactly-once across a
crash boundary is not achievable cheaply, so we do not pretend to provide it.

### Storage failures never destroy accepted events

If the durable write itself fails — a failed `BEGIN`, bind, step, or `COMMIT`,
a full disk, or a locking error — the transaction is rolled back, the connection
is left usable, and the batch is **returned to the front of the queue** for
retry. It keeps its place ahead of newer events. A `flush()` that could not
persist what it drained returns `StorageError` rather than reporting success.

## Ordering

Delivery order is defined by a monotonic **admission ordinal** assigned under the
admission lock, not by the wall clock. A clock that steps backwards, or two
events sharing a millisecond, cannot reorder delivery. The ordinal continues
from the highest stored value after a restart, so recovered events are delivered
before events admitted afterwards.

Per-session sequence numbers (`seq`) are assigned in the same linearized step and
are **gap-free across accepted events**: a rejected event never consumes a
sequence number. A missing `seq` in delivered output therefore indicates a
delivery problem, not back-pressure.

Session markers are ordered with respect to concurrent `track()` calls: a
`session_start` always precedes, and a `session_end` always follows, every event
belonging to that session, even when producers are racing.

## The crash-loss window

There is exactly one window in which accepted events can be lost: **between
acceptance (in the in-memory queue) and the SQLite commit.** A process crash in
that window loses the in-flight, not-yet-persisted events.

Mitigations:

- Keep `batch_interval` small if low latency to disk matters.
- Call `flush(timeout)` at checkpoints (level end, save, before exit). `flush`
  drains the queue into SQLite and attempts delivery before returning, closing
  the window on demand.
- A synchronous-persist mode (persist inside `track()`) is a candidate for v0.2.

Both halves of this contract are covered by tests that terminate a child process
with `_Exit()` — no destructors, no shutdown — and then verify what survived.

## Bounded storage

Durable storage is bounded by `Config::max_pending_events` (default 100,000).
The bound is enforced **at admission**: once that many events are outstanding,
`track()` returns `StorageFull` immediately. Capacity is released only when
events are actually delivered and acknowledged, and events recovered from a
previous run count against the cap from the moment the client opens.

This bounds disk usage if a sink fails permanently (for example an output that
is down for a long time) without silently discarding history. Note that deleting
rows in SQLite returns pages to a free list but does not immediately shrink the
database file; the file stays near its high-water mark until `VACUUM`. v0.1 does
not auto-VACUUM on the hot path. A byte-precise disk quota and a dead-letter
queue are planned for v0.2.

## Shutdown, and why the worker is never abandoned

`shutdown(timeout)` closes admission, calls `Sink::request_cancel()`, drains
what it can, and waits up to `timeout` for the worker to exit.

- `Ok` is returned **only** once the worker has genuinely exited and been
  joined. After that, no further sink or callback invocation is possible.
- On timeout, `Timeout` is returned and the client stays in a stopping state.
  **The worker thread is still owned and still running — it is never detached.**
  Call `shutdown()` again, or `wait_for_worker_exit()`, to observe the real
  state. Undelivered events remain in durable storage either way.
- Every caller — concurrent or later — sees the same truth. A later call cannot
  receive `Ok` while the worker is still inside a sink.

### Why not detach on timeout?

Because `shared_ptr` ownership of the sink and callbacks is **not** enough to
make that safe. PlayerTrace owns the *callable objects*, but not what those
callables **capture**: a sink holding `MyEngine&`, a logger writing to a
caller-owned file, a callback capturing `this`. A detached worker can invoke
that user code after `~Client()` has returned and the surrounding objects are
gone. That is a use-after-free waiting to happen, so the thread is always
joined.

**The honest consequence:** `~Client()` blocks until the worker exits. If a
custom sink refuses to return, destruction blocks with it. There is no portable
way to cancel a thread stuck inside arbitrary user code, so the contract is
cooperative instead:

### Making shutdown bounded with a custom sink

Override `Sink::request_cancel()`. It is called from the shutting-down thread
and may run concurrently with `write()`. Make any in-progress and subsequent
`write()`/`flush()` return promptly — returning a non-ok `Status` is correct,
since the events stay durable and are retried next run.

- **Built-in `FileSink`** implements this and never blocks indefinitely.
- **A cooperative custom sink** gets a genuinely bounded shutdown.
- **A sink that ignores it and blocks forever** will block shutdown's completion
  and eventually `~Client()`. That is a property of the sink, not something the
  SDK can paper over.

Calling `flush()` or `shutdown()` from the worker thread (inside a sink or a
callback) returns `ErrorCode::Internal` rather than dead-locking on a self-join;
`shutdown()` additionally closes admission and requests stop before returning.

## Durable output

`FileSink` returns `Ok` only after the batch has been written, flushed, and
handed to the platform's durable-sync primitive (`fsync`, `F_FULLFSYNC` on
macOS, `_commit` on Windows). Only then does the pipeline delete the
corresponding rows. Durable sync can be turned off explicitly
(`FileSink(path, /*durable_sync=*/false)`), which is faster but means an
acknowledged event can be lost if the machine loses power.

If a write, flush, or sync fails part-way through a batch, the file is truncated
back to the last complete line before the error is reported. A retry therefore
appends clean records instead of following a half-written line, and every
completed line in the file is always valid JSON. A sink whose stream ends up in
a failed state is reopened on the next attempt rather than staying poisoned.

## Fault isolation

One bad input or one bad dependency never crashes the game:

- **Ill-formed UTF-8** — rejected by `track()`/`start_session()` before the
  event is created. Serialization additionally never throws: it returns a
  `Status`, and the worker drops and reports the single offending event.
- **Malformed event** — rejected by validation before it enters the pipeline.
- **Corrupted storage row** — a row whose payload is not valid JSON is
  quarantined during fetch. The quarantine is only reported as successful once
  the deleting transaction commits.
- **Throwing sink** — the worker wraps `Sink::write` and `Sink::flush` in
  try/catch and converts a throw into a `SinkError`, retaining the batch.
- **Throwing callback** — `log`, `error`, and `property_filter` callbacks are
  wrapped; a throw is swallowed (a throwing filter drops the property).
- **Anything else** — the worker's thread entry point has an outermost
  exception barrier, so no failure can terminate the host process by escaping
  the thread.

## Consent interaction

Consent revocation is destructive by design and takes precedence over delivery:
setting consent to anything other than `Granted` discards queued events and
**synchronously purges** pending unsent events before `set_consent` returns. The
delivery guarantee is therefore precisely "at-least-once **while consent remains
Granted**." See [privacy.md](privacy.md) for the full revocation contract.

## Summary table

| Situation | Behavior |
|-----------|----------|
| `track()` returns Ok | Event queued in memory; not yet durable |
| Queue full | `QueueFull` returned synchronously; newest dropped, older kept |
| Durable cap reached | `StorageFull` returned synchronously; newest dropped |
| After SQLite commit | At-least-once delivery; survives restart |
| Duplicate delivery | Possible; dedupe by `event_id` |
| Crash before commit | In-flight queued events lost (the one window) |
| Storage write fails | Batch retained and retried; `flush()` reports `StorageError` |
| Sink fails or throws | Batch retained, retried; contained as `SinkError` |
| Corrupted row | Quarantined; not fatal; reported only if actually removed |
| Shutdown with wedged sink | Returns `Timeout`; worker still owned and running, never abandoned |
| Shutdown with cooperative sink | Bounded via `Sink::request_cancel()`; returns `Ok` once stopped |
| Consent revoked | Queued discarded, session ended, pending purged durably |
| Consent purge fails | `set_consent` returns `StorageError`; collection stays blocked and the purge is retried, including after a restart |
| Two clients on one storage path | Second `create()` fails with `InvalidConfig` |
