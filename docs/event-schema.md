# Event schema

Every event is serialized as a single JSON object. The `FileSink` writes one such
object per line (newline-delimited JSON, "NDJSON"). The serialized form contains
**no trailing newline** at the event level — framing (newlines, arrays, request
envelopes) is the sink's responsibility.

## Top-level fields

| Field | Type | Notes |
|-------|------|-------|
| `event_id` | string (UUIDv4) | Unique per event. Downstream deduplication key. |
| `app_id` | string | From `Config::app_id`. |
| `name` | string | The event name. Validated (see below). |
| `schema_version` | integer | Currently `1`. |
| `timestamp_ms` | integer | UTC epoch milliseconds. Canonical timestamp. |
| `timestamp` | string | ISO-8601 UTC, e.g. `2026-07-24T04:55:01.537Z`. Convenience. |
| `session_id` | string (UUIDv4) | Random per session. |
| `player_id` | string | **Omitted** when no anonymous id was supplied. |
| `seq` | integer | Per-session sequence, starting at 0. Gap-free for accepted events. |
| `properties` | object | Flat key/value map; insertion order preserved. |

## Example

```json
{
  "event_id": "06065f04-8cdb-40b2-875d-30c5707dbd0f",
  "app_id": "simulated-game",
  "name": "player_death",
  "schema_version": 1,
  "timestamp_ms": 1784868901537,
  "timestamp": "2026-07-24T04:55:01.537Z",
  "session_id": "800dde3c-90fa-4a73-831b-c6b58d32fd26",
  "player_id": "anonymous-player-42",
  "seq": 2,
  "properties": {
    "level_id": "forest_01",
    "cause": "fell_off_ledge",
    "x": 128.5,
    "y": 42.0
  }
}
```

## Property values

Properties are a **flat** map (no nested objects or arrays). Each value is one of
four types:

| Type | JSON representation | C++ |
|------|---------------------|-----|
| Bool | `true` / `false` | `bool` |
| Int | integer | `std::int64_t` (and plain `int`) |
| Double | number | `double` (must be finite) |
| String | string | `std::string` / `const char*` |

## Validation rules

Enforced by `EventValidator` before an event enters the pipeline (configurable
via `Config`):

- **Event name**: 1–`max_name_length` chars (default 64); first char `[A-Za-z_]`;
  remaining chars `[A-Za-z0-9._-]`.
- **Property key**: same character rules as event name; must not be a reserved
  key; must be unique within the event.
- **Reserved keys**: the top-level field names above, plus any key beginning with
  the `pt_` prefix (reserved for future SDK-defined properties).
- **Property count**: at most `max_properties` (default 64).
- **String length**: at most `max_string_length` bytes (default 1024).
- **String encoding**: string property values must be **well-formed UTF-8**.
- **Player id**: at most `max_player_id_length` bytes (default 128) and
  well-formed UTF-8. An empty player id means "none supplied" and is allowed.
- **Doubles**: must be finite (`NaN` and infinities are rejected).

Violations return a specific `ErrorCode` from `track()` or `start_session()`
(e.g. `InvalidEventName`, `ReservedKey`, `DuplicateKey`, `TooManyProperties`,
`ValueTooLarge`, `InvalidProperty`) and the event is never enqueued.

Validation runs on the **original** properties, before your `property_filter` is
applied, so a filter cannot mask invalid, duplicate, reserved, or oversized
input.

### On UTF-8

This is a correctness requirement, not a style preference. JSON is defined over
Unicode text, and the serializer rejects ill-formed sequences. Validating at
admission means you receive an `InvalidProperty` status from `track()`, rather
than the problem surfacing later on the background thread. Rejected byte
patterns include stray continuation bytes, truncated sequences, overlong
encodings, UTF-16 surrogate halves, and code points above U+10FFFF.

An embedded NUL (U+0000) *is* well-formed UTF-8 and is accepted. It is escaped
as `\u0000` in the serialized output, never emitted as a raw NUL byte, and is
preserved end to end — durable storage binds payloads with an explicit byte
length, so nothing is silently truncated at the first NUL.

## Standard session events

The SDK emits two standard events automatically (subject to consent):

| Name | When | Properties |
|------|------|------------|
| `session_start` | on `start_session()` | none |
| `session_end` | on `end_session()` | `session_seconds` (double) |

These flow through the same pipeline and carry the same top-level fields as
custom events.

## Recommended custom events

These are conventions, not requirements — you can name events anything valid:

| Name | Suggested properties |
|------|----------------------|
| `level_started` | `level_id`, `difficulty` |
| `level_completed` | `level_id`, `completion_seconds`, `deaths`, `stars` |
| `player_death` | `level_id`, `cause`, `x`, `y` |
| `achievement_unlocked` | `achievement_id`, `hidden` |
| `currency_transaction` | `currency`, `delta`, `reason`, `balance` |

## Schema versioning

The `schema_version` field is stamped on every event.

The SQLite store keeps its own, separate on-disk schema version (currently `2`).
Because v0.1 contains no migration engine, the store accepts **only that exact
version**: a database written by a newer *or* older build is refused with
`StorageError`, as is one whose recorded version is malformed. The version is
inspected before any schema is created, so refusing a database leaves it
byte-for-byte unmodified.
