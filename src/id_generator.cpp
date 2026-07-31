// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#include "internal/id_generator.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace playertrace {
namespace internal {

namespace {
std::uint64_t seed_value() {
  std::random_device rd;
  const std::uint64_t a = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
  const std::uint64_t t = static_cast<std::uint64_t>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
  return a ^ (t * 0x9E3779B97F4A7C15ull);
}
}  // namespace

IdGenerator::IdGenerator() : rng_(seed_value()) {}

std::string IdGenerator::uuid4() {
  std::uint64_t hi;
  std::uint64_t lo;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    hi = rng_();
    lo = rng_();
  }
  // Set the version (4) and variant (10xx) bits per RFC 4122.
  hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
  lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;

  const std::array<std::uint8_t, 16> bytes = {
      static_cast<std::uint8_t>((hi >> 56) & 0xFF),
      static_cast<std::uint8_t>((hi >> 48) & 0xFF),
      static_cast<std::uint8_t>((hi >> 40) & 0xFF),
      static_cast<std::uint8_t>((hi >> 32) & 0xFF),
      static_cast<std::uint8_t>((hi >> 24) & 0xFF),
      static_cast<std::uint8_t>((hi >> 16) & 0xFF),
      static_cast<std::uint8_t>((hi >> 8) & 0xFF),
      static_cast<std::uint8_t>(hi & 0xFF),
      static_cast<std::uint8_t>((lo >> 56) & 0xFF),
      static_cast<std::uint8_t>((lo >> 48) & 0xFF),
      static_cast<std::uint8_t>((lo >> 40) & 0xFF),
      static_cast<std::uint8_t>((lo >> 32) & 0xFF),
      static_cast<std::uint8_t>((lo >> 24) & 0xFF),
      static_cast<std::uint8_t>((lo >> 16) & 0xFF),
      static_cast<std::uint8_t>((lo >> 8) & 0xFF),
      static_cast<std::uint8_t>(lo & 0xFF),
  };

  char buf[37];
  std::snprintf(
      buf, sizeof(buf),
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
      bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13],
      bytes[14], bytes[15]);
  return std::string(buf);
}

}  // namespace internal
}  // namespace playertrace
