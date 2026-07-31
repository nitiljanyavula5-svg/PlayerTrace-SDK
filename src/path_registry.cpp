// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#include "internal/path_registry.hpp"

#include <mutex>
#include <set>
#include <string>
#include <system_error>

#if defined(__has_include)
#if __has_include(<filesystem>)
#include <filesystem>
#define PLAYERTRACE_HAVE_FILESYSTEM 1
#endif
#endif

namespace playertrace {
namespace internal {

namespace {

std::mutex& registry_mutex() {
  static std::mutex m;
  return m;
}

std::set<std::string>& registry() {
  static std::set<std::string> paths;
  return paths;
}

/// True when `path` names a database SQLite will genuinely open IN MEMORY, and
/// which is therefore private per connection and never exclusively owned.
///
/// The exemption is deliberately narrow: it must describe what THIS BUILD's
/// SQLite actually does, not what SQLite can be configured to do.
///
/// URI filenames are an opt-in feature. They are honoured only when the
/// amalgamation is compiled with SQLITE_USE_URI=1, or the database is opened
/// through sqlite3_open_v2() with SQLITE_OPEN_URI. PlayerTrace does neither —
/// see the sqlite3.c COMPILE_DEFINITIONS in CMakeLists.txt and the plain
/// sqlite3_open() call in SqliteStore::open(). So a string beginning "file:" is
/// an ORDINARY FILENAME here: sqlite3_open("file::memory:") creates a disk file
/// literally called "file::memory:".
///
/// Treating those strings as in-memory therefore handed out an exemption SQLite
/// never honoured, and any path containing "mode=memory" silently escaped
/// PathClaim — letting two clients share one real database file undetected,
/// which is precisely what the registry exists to prevent.
///
/// If URI support is ever enabled, this function must be revisited TOGETHER
/// with the open path; the guarded branch below is where that parsing belongs.
bool is_in_memory(const std::string& path) {
#if defined(SQLITE_USE_URI) && SQLITE_USE_URI
#error \
    "SQLite URI filenames are enabled but PathClaim does not parse them; \
in-memory detection in path_registry.cpp must be updated before this builds."
#endif
  // The only spelling this build opens in memory.
  return path == ":memory:";
}

/// Normalizes a path so that "./a.db" and "a.db" collide as they should.
/// Falls back to the raw string when the filesystem library is unavailable or
/// refuses the path; a missed normalization only weakens detection, it never
/// produces a false conflict for genuinely different files.
std::string normalize(const std::string& path) {
#if defined(PLAYERTRACE_HAVE_FILESYSTEM)
  try {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(path, ec);
    if (ec) {
      return path;
    }
    std::filesystem::path canonical = std::filesystem::weakly_canonical(p, ec);
    if (ec) {
      canonical = p.lexically_normal();
    }
    std::string s = canonical.string();
#if defined(_WIN32)
    // Windows paths are case-insensitive; fold so "A.DB" and "a.db" collide.
    for (char& c : s) {
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c - 'A' + 'a');
      }
      if (c == '/') {
        c = '\\';
      }
    }
#endif
    return s;
  } catch (...) {
    return path;
  }
#else
  return path;
#endif
}

const char* kind_name(PathKind kind) {
  return kind == PathKind::Storage ? "storage_path" : "FileSink path";
}

}  // namespace

bool PathClaim::acquire(const std::string& path, PathKind kind,
                        std::string* error) {
  release();
  if (is_in_memory(path)) {
    return true;  // private per connection; nothing to own exclusively
  }

  // ONE namespace for storage and output paths. Keying them separately let a
  // FileSink claim the very file backing the SQLite store, so the sink appended
  // NDJSON into the database while flush() still reported Ok. A path is owned
  // by exactly one component, whatever that component uses it for.
  const std::string key = normalize(path);

  std::lock_guard<std::mutex> lock(registry_mutex());
  if (!registry().insert(key).second) {
    if (error != nullptr) {
      *error = std::string(
                   "another live PlayerTrace component in this process is "
                   "already using this path as a storage_path or a FileSink "
                   "output; it cannot also be used as this ") +
               kind_name(kind) + " ('" + path +
               "'). Storage and output paths must be exclusively owned; use a "
               "distinct path per Client.";
    }
    return false;
  }
  key_ = key;
  held_ = true;
  return true;
}

void PathClaim::release() {
  if (!held_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(registry_mutex());
    registry().erase(key_);
  }
  held_ = false;
  key_.clear();
}

}  // namespace internal
}  // namespace playertrace
