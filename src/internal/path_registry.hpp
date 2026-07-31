// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Internal header — NOT installed and NOT part of the public API.
#ifndef PLAYERTRACE_INTERNAL_PATH_REGISTRY_HPP
#define PLAYERTRACE_INTERNAL_PATH_REGISTRY_HPP

#include <string>
#include <utility>

namespace playertrace {
namespace internal {

/// Kinds of exclusively-owned path.
enum class PathKind { Storage, Output };

/// Process-wide registry of paths that are actively owned by a Client's SQLite
/// store or by a FileSink.
///
/// Two clients sharing one database would keep independent outstanding counts
/// and ordinal generators, deliver the same rows twice, and let one client's
/// consent purge delete the other's events. Two FileSinks on one file would
/// interleave writes and let one instance's rollback truncate the other's
/// output. Neither is detectable at runtime, so the paths are claimed instead.
///
/// The claim is per-process. It cannot see another process using the same file;
/// that remains a documented precondition.
class PathClaim {
 public:
  PathClaim() = default;
  ~PathClaim() { release(); }

  PathClaim(const PathClaim&) = delete;
  PathClaim& operator=(const PathClaim&) = delete;
  PathClaim(PathClaim&& other) noexcept { *this = std::move(other); }
  PathClaim& operator=(PathClaim&& other) noexcept {
    if (this != &other) {
      release();
      key_ = std::move(other.key_);
      held_ = other.held_;
      other.held_ = false;
      other.key_.clear();
    }
    return *this;
  }

  /// Attempts to claim `path`. Returns true on success. On failure `*error`
  /// receives a message naming the conflict. In-memory databases (":memory:"
  /// and any "file::memory:" form) are never exclusive: each connection gets a
  /// private database, so they are always allowed.
  bool acquire(const std::string& path, PathKind kind, std::string* error);

  /// Releases the claim if held. Safe to call repeatedly.
  void release();

  bool held() const noexcept { return held_; }

 private:
  std::string key_;
  bool held_ = false;
};

}  // namespace internal
}  // namespace playertrace

#endif  // PLAYERTRACE_INTERNAL_PATH_REGISTRY_HPP
