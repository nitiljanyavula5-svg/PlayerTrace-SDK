# Privacy

PlayerTrace is designed for privacy-conscious telemetry. The guiding principle is
**data minimization**: the SDK collects only what you explicitly give it, plus
the small amount of metadata needed to make events useful (identifiers,
sequence, timestamp).

## What is collected automatically

Nothing beyond what you pass to `track()` and the metadata the SDK generates:

- `event_id` — a random UUIDv4.
- `session_id` — a random UUIDv4 per session.
- `seq` — a per-session sequence number.
- `timestamp` — UTC time of the event.
- `app_id` — the string you configured.
- `player_id` — only if you supply one (see below).

## What is NOT collected

The SDK never automatically collects, derives, or fingerprints any of the
following:

- Email addresses, real names, or usernames
- IP addresses
- Hardware identifiers (MAC address, serial numbers, disk IDs)
- Advertising identifiers (IDFA, GAID)
- Device fingerprints
- OS/hardware inventory

There is no networking in v0.1 at all, and there are no hardcoded API keys or
server secrets anywhere in the codebase.

## Identifiers are never derived from personal or hardware data

`event_id` and `session_id` are generated from a random source
(`std::random_device` plus a high-resolution time seed). They are **not** derived
from any player, device, or hardware identifier. The optional `player_id` is an
**anonymous** identifier that you, the integrating developer, supply — typically
a randomly generated per-install token. Do not pass an email, account name, or
device id as the `player_id`.

## Hashing is not anonymization

Hashing personal data (e.g. `sha256(email)`) does **not** make it anonymous. A
hash of a low-entropy value is reversible by dictionary/rainbow-table attacks,
and a stable hash is still a persistent identifier that can be linked across
datasets. Do not put hashed personal data into event properties and assume it is
anonymous. Prefer identifiers that carry no personal information at all.

## Consent

Collection is gated by `Config::consent` (`ConsentState`), which you can update
at runtime with `Client::set_consent`.

- The default is `Unknown`. Collection **never** happens unless you explicitly
  set `Granted`. It never defaults to `Granted`.
- While consent is not `Granted`, `track()` returns `ConsentDenied` and the event
  is **never created** — it is not collected-then-deleted.
- `start_session()` is also refused while consent is withheld. A session is not
  opened at all, so there can be no session whose `session_start` was never
  collected but whose `session_end` is emitted later.

### Revocation is destructive, and complete when it returns

`set_consent()` returns a `Status`, because revocation touches durable storage
and can fail. Setting `Denied` (or back to `Unknown`):

1. rejects new events immediately;
2. invalidates every event admitted so far by advancing an internal **consent
   generation**;
3. discards events still waiting in the in-memory queue and in the worker's
   retry buffer;
4. **ends the active session** (without emitting a `session_end`, which would
   describe data being deleted);
5. commits a durable **revocation marker**, then purges pending events and
   clears the marker in a single transaction, before returning;
6. stops the worker from handing further batches to a sink.

**Revocation is fail-closed.** If the purge cannot be committed,
`set_consent()` returns `StorageError`, collection stays blocked, capacity
accounting is *not* reset, and the marker remains on disk. The purge is retried
on the next consent call and again the next time the database is opened. A
re-grant is refused while a purge is still owed, so consent can never be
restored over data that should already be gone.

Three consequences follow, and each is covered by a test:

- **An immediate re-grant cannot revive revoked work.** Events admitted under an
  earlier generation are dropped even if consent becomes `Granted` again a
  microsecond later.
- **An in-flight `track()` cannot cross the boundary.** A call that began before
  revocation — for example one still inside your property filter — is rejected
  with `ConsentDenied` rather than being admitted afterwards.
- **A crash or restart cannot recover the data.** Because the purge is committed
  synchronously, there is no window in which revoked events remain on disk.

Revocation also works after `shutdown()`: the store is still purged even though
the worker has stopped.

If the purge itself fails (a disk error, for example), the failure is reported
through the `error_callback` and logged rather than being silently swallowed.

### The one revocation boundary

The SDK cannot recall bytes a sink has already begun writing. At most **one
in-flight `Sink::write`** may complete after `set_consent(Denied)` returns.

The worker re-checks consent after draining the queue, immediately before each
`Sink::write`, again before acknowledging, and — critically — once more while
holding the storage lock immediately before the durable insert. That last check
is what prevents a batch drained before the revocation from landing in storage
after the purge has already reported success.

**This matters more if you supply your own sink.** `Config::sink` accepts any
`Sink` implementation, including one that sends data over a network. The
built-in `FileSink` writes only to a local file, but PlayerTrace cannot know
what a custom sink does. If your sink transmits data off the machine, that one
in-flight batch may leave the device after revocation returns. Implement
`Sink::request_cancel()` to abort it: that hook is called at the start of both
revocation-driven shutdown and normal shutdown.

## Property filtering

`Config::property_filter` is an optional callback invoked for every property
during `track()`. Return `false` to drop a property (for example, to strip any
key matching a PII pattern) before it is ever recorded. A filter that throws is
treated as "drop the property."

## Your responsibilities

The SDK gives you privacy-preserving defaults, but you control the data you pass
in. Do not put personal data in event names or property values. Use anonymous,
rotating identifiers. Honor player consent choices by driving `set_consent`
accordingly.
