// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Internal header — NOT installed and NOT part of the public API.
#ifndef PLAYERTRACE_INTERNAL_ID_GENERATOR_HPP
#define PLAYERTRACE_INTERNAL_ID_GENERATOR_HPP

#include <mutex>
#include <random>
#include <string>

namespace playertrace {
namespace internal {

/// Generates random RFC-4122 version-4 UUIDs.
///
/// The entropy comes only from std::random_device plus a high-resolution time
/// seed. Identifiers are deliberately NOT derived from player, device, MAC,
/// advertising, or any hardware/personal identifier (see docs/privacy.md).
class IdGenerator {
 public:
  IdGenerator();

  /// Thread-safe. Returns a lowercase, hyphenated UUIDv4 string.
  std::string uuid4();

 private:
  std::mutex mutex_;
  std::mt19937_64 rng_;
};

}  // namespace internal
}  // namespace playertrace

#endif  // PLAYERTRACE_INTERNAL_ID_GENERATOR_HPP
