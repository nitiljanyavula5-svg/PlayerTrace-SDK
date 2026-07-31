// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#include "internal/sqlite_store.hpp"

#include <sqlite3.h>

#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "internal/json_serializer.hpp"

namespace playertrace {
namespace internal {

namespace {

constexpr char kRevocationKey[] = "revocation_pending";

constexpr char kCreateMeta[] =
    "CREATE TABLE IF NOT EXISTS schema_meta ("
    "  key   TEXT PRIMARY KEY NOT NULL,"
    "  value TEXT NOT NULL);";

// event_id is explicitly NOT NULL and non-empty. A rowid table's TEXT PRIMARY
// KEY does NOT imply NOT NULL in SQLite (a documented legacy quirk), and a NULL
// key made acknowledgment silently match nothing, so the same row was delivered
// forever.
constexpr char kCreateEvents[] =
    "CREATE TABLE IF NOT EXISTS events ("
    "  event_id   TEXT PRIMARY KEY NOT NULL CHECK(length(event_id) > 0),"
    "  session_id TEXT NOT NULL,"
    "  seq        INTEGER NOT NULL,"
    "  ordinal    INTEGER NOT NULL,"
    "  created_ms INTEGER NOT NULL,"
    "  payload    TEXT NOT NULL);";

// Delivery order is the monotonic admission ordinal, never the wall clock.
constexpr char kCreateIndex[] =
    "CREATE INDEX IF NOT EXISTS idx_events_order ON events(ordinal);";

/// The exact expected shape of `events`: name, declared type, NOT NULL, pk.
struct ColumnSpec {
  const char* name;
  const char* type;
  int notnull;
  int pk;
};

const ColumnSpec kEventsColumns[] = {
    {"event_id", "TEXT", 1, 1},      {"session_id", "TEXT", 1, 0},
    {"seq", "INTEGER", 1, 0},        {"ordinal", "INTEGER", 1, 0},
    {"created_ms", "INTEGER", 1, 0}, {"payload", "TEXT", 1, 0},
};

/// `schema_meta` carries the version and the revocation marker. Its shape is
/// validated too: a database that stores the marker in a differently-shaped
/// table would silently lose the "purge still owed" evidence.
const ColumnSpec kMetaColumns[] = {
    {"key", "TEXT", 1, 1},
    {"value", "TEXT", 1, 0},
};

std::string to_lower_ascii(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return text;
}

bool is_ident_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '$';
}

/// Replaces every comment and every quoted region of `sql` with a single space,
/// leaving the surrounding SQL structure intact.
///
/// Searching the raw DDL for the constraint text was forgeable: the exact
/// characters `CHECK(length(event_id) > 0)` can be placed inside a string
/// literal, a quoted identifier, or a comment, and a database with NO real
/// constraint would then validate. Blanking those regions first means the
/// subsequent scan can only ever see genuine SQL tokens.
///
/// Handles SQLite's four quoting forms — '...' (with '' escape), "..." (with ""
/// escape), [...] and `...` (with `` escape) — plus -- line and /* */ block
/// comments. An unterminated region blanks to end of input, which is the safe
/// direction: it can only hide text, never invent it.
std::string strip_sql_noise(const std::string& sql) {
  std::string out;
  out.reserve(sql.size());
  std::size_t i = 0;
  const std::size_t n = sql.size();
  while (i < n) {
    const char c = sql[i];
    // -- line comment
    if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
      while (i < n && sql[i] != '\n') {
        ++i;
      }
      out.push_back(' ');
      continue;
    }
    // /* block comment */
    if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) {
        ++i;
      }
      i = (i + 1 < n) ? i + 2 : n;
      out.push_back(' ');
      continue;
    }
    // Quoted regions. `close` is the terminator; for the doubling forms a
    // repeated terminator is an escape and does not end the region.
    if (c == '\'' || c == '"' || c == '`' || c == '[') {
      const char close = (c == '[') ? ']' : c;
      const bool doubling = (c != '[');
      ++i;
      while (i < n) {
        if (sql[i] == close) {
          if (doubling && i + 1 < n && sql[i + 1] == close) {
            i += 2;  // escaped terminator, still inside
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      out.push_back(' ');
      continue;
    }
    out.push_back(c);
    ++i;
  }
  return out;
}

/// Removes all whitespace, so an expression can be compared independently of
/// how it was originally spaced.
std::string collapse_spaces(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' &&
        c != '\v') {
      out.push_back(c);
    }
  }
  return out;
}

/// Collects the normalized text of every CHECK constraint expression in `ddl`.
///
/// `ddl` must already have been passed through strip_sql_noise(). Each `check`
/// is matched as a whole token — not as a substring, so `xcheck` or `checked`
/// cannot trigger it — and its parenthesised expression is taken with balanced
/// nesting rather than by finding the next ')'.
std::vector<std::string> check_expressions(const std::string& ddl) {
  const std::string lowered = to_lower_ascii(ddl);
  std::vector<std::string> found;
  std::size_t i = 0;
  while (true) {
    const std::size_t at = lowered.find("check", i);
    if (at == std::string::npos) {
      break;
    }
    i = at + 5;
    const bool left_ok = (at == 0) || !is_ident_char(lowered[at - 1]);
    const bool right_ok = (i >= lowered.size()) || !is_ident_char(lowered[i]);
    if (!left_ok || !right_ok) {
      continue;  // part of a longer identifier
    }
    std::size_t p = i;
    while (p < lowered.size() && (lowered[p] == ' ' || lowered[p] == '\t' ||
                                  lowered[p] == '\n' || lowered[p] == '\r')) {
      ++p;
    }
    if (p >= lowered.size() || lowered[p] != '(') {
      continue;  // `CHECK` not followed by an expression
    }
    int depth = 0;
    const std::size_t start = p;
    for (; p < lowered.size(); ++p) {
      if (lowered[p] == '(') {
        ++depth;
      } else if (lowered[p] == ')') {
        --depth;
        if (depth == 0) {
          ++p;
          break;
        }
      }
    }
    if (depth != 0) {
      continue;  // unbalanced; not a usable constraint
    }
    // Store without the outer parentheses.
    found.push_back(collapse_spaces(lowered.substr(start + 1, p - start - 2)));
    i = p;
  }
  return found;
}

/// Parses a complete, unsigned decimal schema version. Rejects empty strings,
/// leading/trailing junk, signs, whitespace, and values that overflow int.
bool parse_schema_version(const std::string& text, int* out) {
  if (text.empty() || text.size() > 9) {  // 9 digits cannot overflow int
    return false;
  }
  long value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  if (value > std::numeric_limits<int>::max()) {
    return false;
  }
  *out = static_cast<int>(value);
  return true;
}

/// Binds a std::string with an explicit byte length so embedded NUL bytes are
/// preserved instead of silently truncating the value.
int bind_string(sqlite3_stmt* stmt, int index, const std::string& value) {
  if (value.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return SQLITE_TOOBIG;
  }
  return sqlite3_bind_text(stmt, index, value.data(),
                           static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

/// RAII finalizer so no statement leaks on an early return.
class StmtGuard {
 public:
  explicit StmtGuard(sqlite3_stmt* stmt) : stmt_(stmt) {}
  ~StmtGuard() {
    if (stmt_ != nullptr) {
      sqlite3_finalize(stmt_);
    }
  }
  StmtGuard(const StmtGuard&) = delete;
  StmtGuard& operator=(const StmtGuard&) = delete;

 private:
  sqlite3_stmt* stmt_;
};

std::string column_text(sqlite3_stmt* stmt, int index) {
  const unsigned char* txt = sqlite3_column_text(stmt, index);
  const int bytes = sqlite3_column_bytes(stmt, index);
  if (txt == nullptr || bytes <= 0) {
    return std::string();
  }
  return std::string(reinterpret_cast<const char*>(txt),
                     static_cast<std::size_t>(bytes));
}

}  // namespace

SqliteStore::SqliteStore(std::string path) : path_(std::move(path)) {}

SqliteStore::~SqliteStore() {
  close();
}

Status SqliteStore::last_error(const char* context) const {
  const char* msg = db_ ? sqlite3_errmsg(db_) : "database not open";
  return Status(ErrorCode::StorageError,
                std::string(context) + ": " + (msg ? msg : "unknown error"));
}

Status SqliteStore::exec(const char* sql) {
  char* err = nullptr;
  const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    Status s(ErrorCode::StorageError,
             std::string("exec failed: ") + (err ? err : "unknown"));
    sqlite3_free(err);
    return s;
  }
  return Status();
}

Status SqliteStore::rollback() {
  // A rollback that itself fails leaves the connection inside a transaction:
  // every later statement would then fail confusingly, so it is reported.
  const Status s = exec("ROLLBACK;");
  if (!s.ok() && sqlite3_get_autocommit(db_) == 0) {
    return Status(
        ErrorCode::StorageError,
        "rollback failed and the connection is still in a transaction: " +
            s.message());
  }
  return Status();
}

Status SqliteStore::table_exists(const char* name, bool* exists) const {
  *exists = false;
  const char* q =
      "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, q, -1, &stmt, nullptr) != SQLITE_OK) {
    return last_error("prepare table_exists");
  }
  StmtGuard guard(stmt);
  if (sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    return last_error("bind table_exists");
  }
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    *exists = true;
  } else if (rc != SQLITE_DONE) {
    return last_error("step table_exists");
  }
  return Status();
}

Status SqliteStore::read_schema_version(bool* present, int* version) const {
  *present = false;
  *version = 0;
  const char* q = "SELECT value FROM schema_meta WHERE key='schema_version';";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, q, -1, &stmt, nullptr) != SQLITE_OK) {
    return last_error("prepare schema_version");
  }
  StmtGuard guard(stmt);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const std::string raw = column_text(stmt, 0);
    *present = true;
    if (!parse_schema_version(raw, version)) {
      return Status(ErrorCode::StorageError,
                    "database schema version '" + raw +
                        "' is malformed; refusing to open");
    }
  } else if (rc != SQLITE_DONE) {
    return last_error("step schema_version");
  }
  return Status();
}

/// Confirms the `events` table really has the v3 shape. A matching version
/// number alone is not evidence: a database can claim v3 and still have the
/// wrong columns, which previously failed asynchronously during fetch.
Status SqliteStore::validate_events_shape() const {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "PRAGMA table_info(events);", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    return last_error("prepare table_info");
  }
  StmtGuard guard(stmt);

  constexpr std::size_t kExpected =
      sizeof(kEventsColumns) / sizeof(kEventsColumns[0]);
  std::size_t index = 0;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (index >= kExpected) {
      return Status(ErrorCode::StorageError,
                    "events table has more columns than schema version " +
                        std::to_string(kStoreSchemaVersion) + " defines");
    }
    const ColumnSpec& want = kEventsColumns[index];
    const std::string name = column_text(stmt, 1);
    const std::string type = column_text(stmt, 2);
    const int notnull = sqlite3_column_int(stmt, 3);
    const int pk = sqlite3_column_int(stmt, 5);
    if (name != want.name || type != want.type || notnull != want.notnull ||
        (pk != 0) != (want.pk != 0)) {
      // Built incrementally rather than as one `+` chain: concatenating
      // adjacent runtime strings that way materializes a temporary per
      // operator. The resulting text is byte-for-byte identical.
      std::string message = "events column ";
      message += std::to_string(index);
      message += " is '";
      message += name;
      message += ' ';
      message += type;
      message += "' but schema version ";
      message += std::to_string(kStoreSchemaVersion);
      message += " requires '";
      message += want.name;
      message += ' ';
      message += want.type;
      message += "'; refusing to open";
      return Status(ErrorCode::StorageError, message);
    }
    ++index;
  }
  if (rc != SQLITE_DONE) {
    return last_error("step table_info");
  }
  if (index != kExpected) {
    return Status(ErrorCode::StorageError,
                  "events table has " + std::to_string(index) +
                      " column(s); schema version " +
                      std::to_string(kStoreSchemaVersion) + " requires " +
                      std::to_string(kExpected) + "; refusing to open");
  }

  // A matching column list still does not prove the non-empty guarantee:
  // PRAGMA table_info does not report CHECK constraints. Without this the
  // event_id CHECK could be missing and empty identifiers would be accepted,
  // which is exactly the redelivery-forever bug v3 exists to prevent.
  Status check = validate_events_check();
  if (!check.ok()) {
    return check;
  }

  // The delivery-order index must exist AND actually index `ordinal`. Checking
  // only that the NAME exists would accept an index over the wrong column, and
  // delivery order would silently degrade to whatever SQLite chose.
  return validate_order_index();
}

/// Confirms the stored CREATE TABLE for `events` still carries the non-empty
/// event_id CHECK. Read-only: it reads sqlite_master.sql.
Status SqliteStore::validate_events_check() const {
  sqlite3_stmt* stmt = nullptr;
  const char* q =
      "SELECT sql FROM sqlite_master WHERE type='table' AND name='events';";
  if (sqlite3_prepare_v2(db_, q, -1, &stmt, nullptr) != SQLITE_OK) {
    return last_error("prepare events sql");
  }
  StmtGuard guard(stmt);
  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    if (rc == SQLITE_DONE) {
      return Status(ErrorCode::StorageError,
                    "database is missing the events table definition; refusing "
                    "to open");
    }
    return last_error("step events sql");
  }
  // Blank comments and quoted regions FIRST, then scan tokens. Matching the raw
  // DDL let the constraint text be forged inside a string literal or a comment
  // while no real constraint existed.
  const std::string ddl = strip_sql_noise(column_text(stmt, 0));
  const std::vector<std::string> checks = check_expressions(ddl);

  // The EXPRESSION is compared, not merely the presence of the word CHECK: a
  // constraint such as length(event_id) > 1 would otherwise pass.
  bool satisfied = false;
  for (const auto& expr : checks) {
    if (expr == "length(event_id)>0" || expr == "0<length(event_id)") {
      satisfied = true;
      break;
    }
  }
  if (!satisfied) {
    std::string seen;
    for (const auto& expr : checks) {
      // Appended in place. `seen += sep + expr` built a temporary string per
      // iteration; the separator and the expression are appended directly
      // instead. The resulting text is identical.
      if (!seen.empty()) {
        seen += ", ";
      }
      seen += expr;
    }
    return Status(ErrorCode::StorageError,
                  "events.event_id is missing the non-empty CHECK constraint "
                  "required by schema version " +
                      std::to_string(kStoreSchemaVersion) + " (found " +
                      (checks.empty() ? std::string("no CHECK constraint")
                                      : "CHECK(" + seen + ")") +
                      ")" + "; refusing to open");
  }
  return Status();
}

/// Confirms idx_events_order is a real index over exactly the `ordinal` column
/// of `events`, not merely a name that happens to exist.
Status SqliteStore::validate_order_index() const {
  {
    sqlite3_stmt* stmt = nullptr;
    const char* q =
        "SELECT tbl_name FROM sqlite_master WHERE type='index' AND name=?;";
    if (sqlite3_prepare_v2(db_, q, -1, &stmt, nullptr) != SQLITE_OK) {
      return last_error("prepare index check");
    }
    StmtGuard guard(stmt);
    if (sqlite3_bind_text(stmt, 1, "idx_events_order", -1, SQLITE_TRANSIENT) !=
        SQLITE_OK) {
      return last_error("bind index check");
    }
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
      return Status(
          ErrorCode::StorageError,
          "database is missing the idx_events_order index; refusing to open");
    }
    if (rc != SQLITE_ROW) {
      return last_error("step index check");
    }
    const std::string table = column_text(stmt, 0);
    if (table != "events") {
      return Status(ErrorCode::StorageError,
                    "idx_events_order indexes table '" + table +
                        "' rather than 'events'; refusing to open");
    }
  }

  // PRAGMA index_list reports what the index REALLY is, straight from the
  // schema, so none of this can be forged in the stored DDL text. A partial
  // index would silently exclude rows from the delivery order, and an
  // auto-index or a unique index is not the object this schema defines.
  {
    sqlite3_stmt* list = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA index_list(events);", -1, &list,
                           nullptr) != SQLITE_OK) {
      return last_error("prepare index_list");
    }
    StmtGuard list_guard(list);
    bool seen = false;
    int rc_list = SQLITE_OK;
    while ((rc_list = sqlite3_step(list)) == SQLITE_ROW) {
      if (column_text(list, 1) != "idx_events_order") {
        continue;
      }
      seen = true;
      const int is_unique = sqlite3_column_int(list, 2);
      const std::string origin = column_text(list, 3);
      const int is_partial = sqlite3_column_int(list, 4);
      if (is_partial != 0) {
        return Status(ErrorCode::StorageError,
                      "idx_events_order is a PARTIAL index; schema version " +
                          std::to_string(kStoreSchemaVersion) +
                          " requires it to cover every row; refusing to open");
      }
      if (is_unique != 0) {
        return Status(ErrorCode::StorageError,
                      "idx_events_order is UNIQUE; schema version " +
                          std::to_string(kStoreSchemaVersion) +
                          " defines a non-unique index; refusing to open");
      }
      if (origin != "c") {
        return Status(
            ErrorCode::StorageError,
            "idx_events_order was not created by CREATE INDEX (origin '" +
                origin + "'); refusing to open");
      }
      break;
    }
    if (rc_list != SQLITE_ROW && rc_list != SQLITE_DONE) {
      return last_error("step index_list");
    }
    if (!seen) {
      return Status(
          ErrorCode::StorageError,
          "database is missing the idx_events_order index; refusing to open");
    }
  }

  sqlite3_stmt* info = nullptr;
  if (sqlite3_prepare_v2(db_, "PRAGMA index_info(idx_events_order);", -1, &info,
                         nullptr) != SQLITE_OK) {
    return last_error("prepare index_info");
  }
  StmtGuard info_guard(info);
  std::size_t columns = 0;
  std::string first;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(info)) == SQLITE_ROW) {
    if (columns == 0) {
      first = column_text(info, 2);  // column name
    }
    ++columns;
  }
  if (rc != SQLITE_DONE) {
    return last_error("step index_info");
  }
  if (columns != 1 || first != "ordinal") {
    return Status(ErrorCode::StorageError,
                  "idx_events_order must index exactly the 'ordinal' column "
                  "but indexes " +
                      std::to_string(columns) + " column(s) starting with '" +
                      first + "'; refusing to open");
  }
  return Status();
}

/// Confirms `schema_meta` has the exact v3 shape.
Status SqliteStore::validate_meta_shape() const {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "PRAGMA table_info(schema_meta);", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    return last_error("prepare meta table_info");
  }
  StmtGuard guard(stmt);
  constexpr std::size_t kExpected =
      sizeof(kMetaColumns) / sizeof(kMetaColumns[0]);
  std::size_t index = 0;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (index >= kExpected) {
      return Status(ErrorCode::StorageError,
                    "schema_meta has more columns than schema version " +
                        std::to_string(kStoreSchemaVersion) +
                        " defines; refusing to open");
    }
    const ColumnSpec& want = kMetaColumns[index];
    const std::string name = column_text(stmt, 1);
    const std::string type = column_text(stmt, 2);
    const int notnull = sqlite3_column_int(stmt, 3);
    const int pk = sqlite3_column_int(stmt, 5);
    if (name != want.name || type != want.type || notnull != want.notnull ||
        (pk != 0) != (want.pk != 0)) {
      // Same incremental construction as validate_events_shape, and the same
      // reason: no temporary per concatenation, identical output text.
      std::string message = "schema_meta column ";
      message += std::to_string(index);
      message += " is '";
      message += name;
      message += ' ';
      message += type;
      message += "' but schema version ";
      message += std::to_string(kStoreSchemaVersion);
      message += " requires '";
      message += want.name;
      message += ' ';
      message += want.type;
      message += "'; refusing to open";
      return Status(ErrorCode::StorageError, message);
    }
    ++index;
  }
  if (rc != SQLITE_DONE) {
    return last_error("step meta table_info");
  }
  if (index != kExpected) {
    return Status(ErrorCode::StorageError,
                  "schema_meta has " + std::to_string(index) +
                      " column(s); schema version " +
                      std::to_string(kStoreSchemaVersion) + " requires " +
                      std::to_string(kExpected) + "; refusing to open");
  }
  return Status();
}

Status SqliteStore::open() {
  const int rc = sqlite3_open(path_.c_str(), &db_);
  if (rc != SQLITE_OK) {
    return last_error("open");
  }

  // Schema inspection happens FIRST and mutates nothing: reading sqlite_master
  // and PRAGMA table_info are read-only. Setting journal_mode IS a mutation, so
  // it must not run until the database has been accepted — otherwise refusing a
  // future-version database would still have rewritten its journal mode.
  Status schema = inspect_and_prepare_schema();
  if (!schema.ok()) {
    close();
    return schema;
  }

  {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA journal_mode=WAL;", -1, &stmt,
                           nullptr) != SQLITE_OK) {
      return last_error("prepare journal_mode");
    }
    StmtGuard guard(stmt);
    const int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
      journal_mode_ = column_text(stmt, 0);
    } else if (step != SQLITE_DONE) {
      return last_error("step journal_mode");
    }
  }
  Status s = exec("PRAGMA synchronous=NORMAL;");
  if (!s.ok())
    return s;
  s = exec("PRAGMA busy_timeout=5000;");
  if (!s.ok())
    return s;
  return Status();
}

Status SqliteStore::inspect_and_prepare_schema() {
  bool meta_exists = false;
  Status s = table_exists("schema_meta", &meta_exists);
  if (!s.ok()) {
    return s;
  }

  if (meta_exists) {
    // Validate the metadata table's own shape before trusting anything it
    // says. All of this is read-only, so a refusal leaves the file untouched.
    s = validate_meta_shape();
    if (!s.ok()) {
      return s;
    }
    bool present = false;
    int version = 0;
    s = read_schema_version(&present, &version);
    if (!s.ok()) {
      return s;  // malformed version: nothing has been modified
    }
    if (!present) {
      return Status(ErrorCode::StorageError,
                    "database has a schema_meta table but no schema_version "
                    "row; refusing to open");
    }
    if (version != kStoreSchemaVersion) {
      return Status(ErrorCode::StorageError,
                    "database schema version " + std::to_string(version) +
                        " is not supported (this build requires version " +
                        std::to_string(kStoreSchemaVersion) +
                        "); refusing to open");
    }
    bool events_exists = false;
    s = table_exists("events", &events_exists);
    if (!s.ok())
      return s;
    if (!events_exists) {
      return Status(ErrorCode::StorageError,
                    "database is missing the events table; refusing to open");
    }
    return validate_events_shape();
  }

  // No schema_meta: either a brand-new database, or something we do not own.
  bool events_exists = false;
  s = table_exists("events", &events_exists);
  if (!s.ok()) {
    return s;
  }
  if (events_exists) {
    return Status(ErrorCode::StorageError,
                  "database contains an events table with no schema_meta; "
                  "refusing to open an unrecognized database");
  }

  s = exec("BEGIN IMMEDIATE;");
  if (!s.ok())
    return s;
  s = exec(kCreateMeta);
  if (!s.ok()) {
    rollback();
    return s;
  }
  s = exec(kCreateEvents);
  if (!s.ok()) {
    rollback();
    return s;
  }
  s = exec(kCreateIndex);
  if (!s.ok()) {
    rollback();
    return s;
  }
  const std::string ins =
      "INSERT INTO schema_meta(key, value) VALUES('schema_version','" +
      std::to_string(kStoreSchemaVersion) + "');";
  s = exec(ins.c_str());
  if (!s.ok()) {
    rollback();
    return s;
  }
  s = exec("COMMIT;");
  if (!s.ok()) {
    rollback();
    return s;
  }
  return Status();
}

Status SqliteStore::insert(const std::vector<StoredRecord>& records) {
  if (records.empty()) {
    return Status();
  }
  if (fault("begin")) {
    return Status(ErrorCode::StorageError, "injected fault: begin");
  }
  Status begin = exec("BEGIN IMMEDIATE;");
  if (!begin.ok()) {
    return begin;
  }

  const char* sql =
      "INSERT OR IGNORE INTO events"
      "(event_id, session_id, seq, ordinal, created_ms, payload) "
      "VALUES(?,?,?,?,?,?);";
  if (fault("prepare_insert")) {
    rollback();
    return Status(ErrorCode::StorageError, "injected fault: prepare_insert");
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    Status s = last_error("prepare insert");
    rollback();
    return s;
  }
  StmtGuard guard(stmt);

  for (const auto& r : records) {
    if (fault("bind_insert")) {
      rollback();
      return Status(ErrorCode::StorageError, "injected fault: bind_insert");
    }
    if (bind_string(stmt, 1, r.event_id) != SQLITE_OK ||
        bind_string(stmt, 2, r.session_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(r.sequence)) !=
            SQLITE_OK ||
        sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(r.ordinal)) !=
            SQLITE_OK ||
        sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(r.created_ms)) !=
            SQLITE_OK ||
        bind_string(stmt, 6, r.payload) != SQLITE_OK) {
      Status s = last_error("bind insert");
      rollback();
      return s;
    }
    if (fault("step_insert")) {
      rollback();
      return Status(ErrorCode::StorageError, "injected fault: step_insert");
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      Status s = last_error("step insert");
      rollback();
      return s;
    }
    if (sqlite3_reset(stmt) != SQLITE_OK) {
      Status s = last_error("reset insert");
      rollback();
      return s;
    }
    if (sqlite3_clear_bindings(stmt) != SQLITE_OK) {
      Status s = last_error("clear bindings");
      rollback();
      return s;
    }
  }

  if (fault("commit")) {
    rollback();
    return Status(ErrorCode::StorageError, "injected fault: commit");
  }
  Status commit = exec("COMMIT;");
  if (!commit.ok()) {
    Status rb = rollback();
    if (!rb.ok()) {
      return rb;
    }
    return commit;
  }
  return Status();
}

Status SqliteStore::scalar_uint64(const char* sql, std::uint64_t bind_value,
                                  bool use_bind, std::uint64_t* out) const {
  *out = 0;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return last_error("prepare scalar");
  }
  StmtGuard guard(stmt);
  if (use_bind &&
      sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(bind_value)) !=
          SQLITE_OK) {
    return last_error("bind scalar");
  }
  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    return last_error("step scalar");
  }
  const sqlite3_int64 value = sqlite3_column_int64(stmt, 0);
  *out = value > 0 ? static_cast<std::uint64_t>(value) : 0;
  return Status();
}

Status SqliteStore::count(std::size_t* out) const {
  if (out == nullptr) {
    return Status(ErrorCode::Internal, "count: null output");
  }
  *out = 0;
  if (fault("count")) {
    return Status(ErrorCode::StorageError, "injected fault: count");
  }
  std::uint64_t value = 0;
  Status s = scalar_uint64("SELECT COUNT(*) FROM events;", 0, false, &value);
  if (!s.ok()) {
    return s;
  }
  *out = static_cast<std::size_t>(value);
  return Status();
}

Status SqliteStore::max_ordinal(std::uint64_t* out) const {
  if (out == nullptr) {
    return Status(ErrorCode::Internal, "max_ordinal: null output");
  }
  return scalar_uint64("SELECT COALESCE(MAX(ordinal), 0) FROM events;", 0,
                       false, out);
}

Status SqliteStore::count_up_to(std::uint64_t barrier, std::size_t* out) const {
  if (out == nullptr) {
    return Status(ErrorCode::Internal, "count_up_to: null output");
  }
  *out = 0;
  if (fault("count")) {
    return Status(ErrorCode::StorageError, "injected fault: count");
  }
  std::uint64_t value = 0;
  Status s = scalar_uint64("SELECT COUNT(*) FROM events WHERE ordinal <= ?;",
                           barrier, true, &value);
  if (!s.ok()) {
    return s;
  }
  *out = static_cast<std::size_t>(value);
  return Status();
}

FetchResult SqliteStore::fetch_pending(std::size_t limit) {
  FetchResult result;
  if (fault("prepare_fetch")) {
    result.status = Status(ErrorCode::StorageError, "injected fault: fetch");
    return result;
  }
  if (limit > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    limit = static_cast<std::size_t>(std::numeric_limits<int>::max());
  }
  // rowid is selected so a row with an unusable key can still be removed.
  const char* q =
      "SELECT rowid, event_id, payload FROM events ORDER BY ordinal LIMIT ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, q, -1, &stmt, nullptr) != SQLITE_OK) {
    result.status = last_error("prepare fetch");
    return result;
  }
  StmtGuard guard(stmt);
  if (sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(limit)) !=
      SQLITE_OK) {
    result.status = last_error("bind fetch");
    return result;
  }

  std::vector<std::int64_t> corrupt_rowids;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const std::int64_t rowid =
        static_cast<std::int64_t>(sqlite3_column_int64(stmt, 0));
    const bool id_is_null = sqlite3_column_type(stmt, 1) == SQLITE_NULL;
    std::string id_str = column_text(stmt, 1);
    std::string payload_str = column_text(stmt, 2);

    // An unusable identifier can never be acknowledged (DELETE ... WHERE
    // event_id = '' matches nothing), which used to redeliver the row forever.
    if (id_is_null || id_str.empty() ||
        !JsonSerializer::is_valid_json(payload_str)) {
      corrupt_rowids.push_back(rowid);
      continue;
    }
    result.events.push_back(
        SerializedEvent{std::move(id_str), std::move(payload_str)});
  }
  if (rc != SQLITE_DONE) {
    result.status = last_error("step fetch");
    return result;
  }

  if (!corrupt_rowids.empty()) {
    std::size_t removed = 0;
    const Status deleted = delete_rowids(corrupt_rowids, &removed);
    if (deleted.ok() && removed == corrupt_rowids.size()) {
      result.quarantined = removed;
      result.status =
          Status(ErrorCode::StorageError,
                 "quarantined " + std::to_string(removed) +
                     " unusable row(s) (bad identifier or payload)");
    } else {
      // Do not claim a successful quarantine, and do not release capacity.
      result.status = Status(
          ErrorCode::StorageError,
          "found " + std::to_string(corrupt_rowids.size()) +
              " unusable row(s) but could not quarantine them: " +
              (deleted.ok()
                   ? std::string("only ") + std::to_string(removed) + " removed"
                   : deleted.message()));
    }
  }
  return result;
}

Status SqliteStore::delete_rowids(const std::vector<std::int64_t>& rowids,
                                  std::size_t* deleted) {
  *deleted = 0;
  if (rowids.empty()) {
    return Status();
  }
  if (fault("begin_quarantine")) {
    return Status(ErrorCode::StorageError, "injected fault: begin_quarantine");
  }
  Status begin = exec("BEGIN IMMEDIATE;");
  if (!begin.ok()) {
    return begin;
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM events WHERE rowid = ?;", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    Status s = last_error("prepare delete rowid");
    rollback();
    return s;
  }
  StmtGuard guard(stmt);
  std::size_t removed = 0;
  for (std::int64_t rowid : rowids) {
    if (sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(rowid)) !=
        SQLITE_OK) {
      Status s = last_error("bind delete rowid");
      rollback();
      return s;
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      Status s = last_error("step delete rowid");
      rollback();
      return s;
    }
    removed += static_cast<std::size_t>(sqlite3_changes(db_));
    if (sqlite3_reset(stmt) != SQLITE_OK) {
      Status s = last_error("reset delete rowid");
      rollback();
      return s;
    }
    sqlite3_clear_bindings(stmt);
  }
  Status commit = exec("COMMIT;");
  if (!commit.ok()) {
    rollback();
    return commit;
  }
  *deleted = removed;
  return Status();
}

Status SqliteStore::delete_ids(const std::vector<std::string>& event_ids,
                               std::size_t* deleted) {
  *deleted = 0;
  if (event_ids.empty()) {
    return Status();
  }
  if (fault("begin_delete")) {
    return Status(ErrorCode::StorageError, "injected fault: begin_delete");
  }
  Status begin = exec("BEGIN IMMEDIATE;");
  if (!begin.ok()) {
    return begin;
  }

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM events WHERE event_id = ?;", -1,
                         &stmt, nullptr) != SQLITE_OK) {
    Status s = last_error("prepare delete");
    rollback();
    return s;
  }
  StmtGuard guard(stmt);
  std::size_t removed = 0;
  for (const auto& id : event_ids) {
    if (bind_string(stmt, 1, id) != SQLITE_OK) {
      Status s = last_error("bind delete");
      rollback();
      return s;
    }
    if (fault("step_delete")) {
      rollback();
      return Status(ErrorCode::StorageError, "injected fault: step_delete");
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      Status s = last_error("step delete");
      rollback();
      return s;
    }
    // Verify the row really went away; a silent zero-row delete used to be
    // reported as a successful acknowledgment.
    removed += static_cast<std::size_t>(sqlite3_changes(db_));
    if (sqlite3_reset(stmt) != SQLITE_OK) {
      Status s = last_error("reset delete");
      rollback();
      return s;
    }
    if (sqlite3_clear_bindings(stmt) != SQLITE_OK) {
      Status s = last_error("clear bindings");
      rollback();
      return s;
    }
  }
  if (fault("commit_delete")) {
    rollback();
    return Status(ErrorCode::StorageError, "injected fault: commit_delete");
  }
  Status commit = exec("COMMIT;");
  if (!commit.ok()) {
    rollback();
    return commit;
  }
  *deleted = removed;
  return Status();
}

Status SqliteStore::acknowledge(const std::vector<std::string>& event_ids,
                                std::size_t* deleted) {
  std::size_t local = 0;
  std::size_t* target = deleted != nullptr ? deleted : &local;
  Status s = delete_ids(event_ids, target);
  if (!s.ok()) {
    return s;
  }
  if (*target != event_ids.size()) {
    return Status(ErrorCode::StorageError,
                  "acknowledgment removed " + std::to_string(*target) + " of " +
                      std::to_string(event_ids.size()) +
                      " row(s); the store and the batch disagree");
  }
  return Status();
}

Status SqliteStore::mark_revocation_pending() {
  if (fault("mark_revocation")) {
    return Status(ErrorCode::StorageError, "injected fault: mark_revocation");
  }
  Status begin = exec("BEGIN IMMEDIATE;");
  if (!begin.ok()) {
    return begin;
  }
  const std::string sql =
      std::string("INSERT OR REPLACE INTO schema_meta(key, value) VALUES('") +
      kRevocationKey + "','1');";
  Status s = exec(sql.c_str());
  if (!s.ok()) {
    rollback();
    return s;
  }
  Status commit = exec("COMMIT;");
  if (!commit.ok()) {
    rollback();
    return commit;
  }
  return Status();
}

Status SqliteStore::purge_and_clear_revocation(std::size_t* deleted) {
  std::size_t local = 0;
  std::size_t* target = deleted != nullptr ? deleted : &local;
  *target = 0;
  if (fault("purge")) {
    return Status(ErrorCode::StorageError, "injected fault: purge");
  }
  Status begin = exec("BEGIN IMMEDIATE;");
  if (!begin.ok()) {
    return begin;
  }
  Status del = exec("DELETE FROM events;");
  if (!del.ok()) {
    rollback();
    return del;
  }
  const std::size_t removed = static_cast<std::size_t>(sqlite3_changes(db_));
  const std::string clear = std::string("DELETE FROM schema_meta WHERE key='") +
                            kRevocationKey + "';";
  Status cleared = exec(clear.c_str());
  if (!cleared.ok()) {
    rollback();
    return cleared;
  }
  if (fault("commit_purge")) {
    rollback();
    return Status(ErrorCode::StorageError, "injected fault: commit_purge");
  }
  Status commit = exec("COMMIT;");
  if (!commit.ok()) {
    rollback();
    return commit;
  }
  *target = removed;
  return Status();
}

Status SqliteStore::revocation_pending(bool* out) const {
  if (out == nullptr) {
    return Status(ErrorCode::Internal, "revocation_pending: null output");
  }
  *out = false;
  sqlite3_stmt* stmt = nullptr;
  const char* q = "SELECT 1 FROM schema_meta WHERE key=? LIMIT 1;";
  if (sqlite3_prepare_v2(db_, q, -1, &stmt, nullptr) != SQLITE_OK) {
    return last_error("prepare revocation_pending");
  }
  StmtGuard guard(stmt);
  if (sqlite3_bind_text(stmt, 1, kRevocationKey, -1, SQLITE_TRANSIENT) !=
      SQLITE_OK) {
    return last_error("bind revocation_pending");
  }
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    *out = true;
  } else if (rc != SQLITE_DONE) {
    return last_error("step revocation_pending");
  }
  return Status();
}

Status SqliteStore::close() {
  if (db_ == nullptr) {
    return Status();
  }
  const int rc = sqlite3_close(db_);
  if (rc != SQLITE_OK) {
    // SQLITE_BUSY here means a statement was left unfinalized. The handle is
    // deliberately not reused after a failed close.
    Status s = last_error("close");
    db_ = nullptr;
    return s;
  }
  db_ = nullptr;
  return Status();
}

}  // namespace internal
}  // namespace playertrace
