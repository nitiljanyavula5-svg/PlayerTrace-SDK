// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Internal header — NOT installed and NOT part of the public API. This is the
// ONLY place <sqlite3.h> is used; it never appears in any public header.
#ifndef PLAYERTRACE_INTERNAL_SQLITE_STORE_HPP
#define PLAYERTRACE_INTERNAL_SQLITE_STORE_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "playertrace/result.hpp"
#include "playertrace/sink.hpp"

struct sqlite3;  // forward declaration; the header pulls in no SQLite types.

namespace playertrace {
namespace internal {

/// The current on-disk schema version. Only this exact version is accepted, and
/// its exact shape is validated: there is no migration engine in v0.1, so both
/// newer and older databases are refused rather than reinterpreted.
///
/// v3 adds: event_id NOT NULL plus a non-empty CHECK (a rowid table's TEXT
/// PRIMARY KEY otherwise permits NULL, which produced an infinite redelivery
/// loop), and the durable consent-revocation marker.
constexpr int kStoreSchemaVersion = 3;

/// One row ready for durable insertion (already serialized).
struct StoredRecord {
  std::string event_id;
  std::string session_id;
  std::uint64_t sequence = 0;
  std::uint64_t ordinal = 0;  ///< Monotonic admission ordinal; delivery order.
  std::int64_t created_ms = 0;
  std::string payload;  ///< Full serialized JSON (the SerializedEvent.json).
};

/// Result of fetching pending events for delivery.
struct FetchResult {
  Status status;
  std::vector<SerializedEvent> events;
  /// Rows removed because their identifier or payload was unusable. Reported
  /// only once the deleting transaction commits, so the caller can release the
  /// matching capacity exactly once.
  std::size_t quarantined = 0;
};

/// Test-only hook allowing deterministic fault injection. Returns true to make
/// the named operation fail as though SQLite had reported an error.
using FaultHook = std::function<bool(const char* operation)>;

/// A durable, SQLite-backed store of pending (not-yet-delivered) events.
class SqliteStore {
 public:
  explicit SqliteStore(std::string path);
  ~SqliteStore();

  SqliteStore(const SqliteStore&) = delete;
  SqliteStore& operator=(const SqliteStore&) = delete;

  /// Opens the database and validates it.
  ///
  /// The schema is inspected READ-ONLY before any mutating statement — the
  /// journal-mode pragma included — so refusing an unsupported database leaves
  /// its contents untouched. A database at the supported version additionally
  /// has its exact table and index shape validated, rather than being trusted
  /// because a version number matched.
  Status open();

  /// Inserts records in a single transaction. Idempotent per event_id.
  Status insert(const std::vector<StoredRecord>& records);

  /// Number of pending events. A failure is never reported as "zero pending".
  Status count(std::size_t* out) const;

  /// Highest admission ordinal currently stored, or 0 when empty.
  Status max_ordinal(std::uint64_t* out) const;

  /// Number of pending events whose ordinal is <= `barrier`. Used to decide
  /// whether any event admitted before a flush began is still outstanding.
  Status count_up_to(std::uint64_t barrier, std::size_t* out) const;

  /// Returns up to `limit` pending events in admission (ordinal) order. Rows
  /// with an unusable identifier or an unparseable payload are quarantined by
  /// rowid; the count is reported only after that deletion commits.
  FetchResult fetch_pending(std::size_t limit);

  /// Deletes the given events after a sink acknowledged them. `*deleted`
  /// receives the number of rows actually removed, verified with
  /// sqlite3_changes(); a delete that matched nothing is reported as an error
  /// rather than as a successful acknowledgment.
  Status acknowledge(const std::vector<std::string>& event_ids,
                     std::size_t* deleted);

  // --- durable consent revocation ------------------------------------------

  /// Commits the revocation marker. Once this returns Ok, a crash or restart
  /// before the purge finishes still leaves evidence that a purge is owed.
  Status mark_revocation_pending();

  /// Deletes every pending event AND clears the revocation marker in one
  /// transaction, so the marker survives exactly until the purge is durable.
  Status purge_and_clear_revocation(std::size_t* deleted);

  /// True when a previous run recorded a revocation that never completed.
  Status revocation_pending(bool* out) const;

  /// The journal mode SQLite actually selected (e.g. "wal", "memory").
  const std::string& journal_mode() const noexcept { return journal_mode_; }

  /// Test-only. Installs a fault hook; pass nullptr to clear.
  void set_fault_hook(FaultHook hook) { fault_hook_ = std::move(hook); }

  /// Closes the connection, reporting a failure to close cleanly.
  Status close();

 private:
  Status exec(const char* sql);
  Status rollback();  ///< Rolls back; reports if the connection is poisoned.
  Status inspect_and_prepare_schema();
  Status validate_events_shape() const;
  /// Confirms the stored CREATE TABLE still carries the non-empty event_id
  /// CHECK; PRAGMA table_info cannot see constraints.
  Status validate_events_check() const;
  /// Confirms idx_events_order really indexes events(ordinal), rather than
  /// merely existing under that name.
  Status validate_order_index() const;
  /// Confirms schema_meta has the exact v3 shape, so the durable revocation
  /// marker cannot be stored somewhere that silently loses it.
  Status validate_meta_shape() const;
  Status read_schema_version(bool* present, int* version) const;
  Status table_exists(const char* name, bool* exists) const;
  Status scalar_uint64(const char* sql, std::uint64_t bind_value, bool use_bind,
                       std::uint64_t* out) const;
  Status delete_ids(const std::vector<std::string>& event_ids,
                    std::size_t* deleted);
  Status delete_rowids(const std::vector<std::int64_t>& rowids,
                       std::size_t* deleted);
  Status last_error(const char* context) const;
  bool fault(const char* operation) const {
    return fault_hook_ && fault_hook_(operation);
  }

  std::string path_;
  sqlite3* db_ = nullptr;
  std::string journal_mode_;
  FaultHook fault_hook_;
};

}  // namespace internal
}  // namespace playertrace

#endif  // PLAYERTRACE_INTERNAL_SQLITE_STORE_HPP
