// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef PLAYERTRACE_CONFIG_HPP
#define PLAYERTRACE_CONFIG_HPP

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "consent.hpp"
#include "event.hpp"
#include "logger.hpp"

namespace playertrace {

class Sink;

/// Optional callback that decides whether a property is kept. Return true to
/// keep the property, false to drop it (a privacy filter for stripping keys
/// that may carry personal data). Invoked on the calling thread inside track().
using PropertyFilter =
    std::function<bool(const std::string& key, const PropertyValue& value)>;

/// Immutable-after-create configuration for a Client. Passed by value to
/// Client::create(). Reasonable defaults are provided for everything except
/// `app_id` and `storage_path`.
struct Config {
  // --- Required ---
  std::string app_id;  ///< Identifier for the game/app. [A-Za-z0-9._-], <= 64.
  std::string
      storage_path;  ///< Path to the SQLite database file. Use a real
                     ///< path in production. ":memory:" is supported for
                     ///< tests only and is not durable across restarts.

  // --- Consent (defaults to Unknown; never defaults to Granted) ---
  ConsentState consent = ConsentState::Unknown;

  // --- Output ---
  std::shared_ptr<Sink> sink;  ///< Destination sink. If null, a FileSink at
                               ///< "<app_id>-events.ndjson" is created.

  // --- In-memory queue / batching ---
  std::size_t max_queue_size = 10000;  ///< Bound on the in-memory queue.
  std::size_t batch_size = 100;        ///< Events processed per worker batch.
  std::chrono::milliseconds batch_interval{
      1000};  ///< Max wait before a partial
              ///< batch is processed.

  // --- Durable storage bound (count cap, not a byte quota) ---
  std::size_t max_pending_events =
      100000;  ///< Max unsent events kept in SQLite.
               ///< Beyond this, the newest incoming
               ///< events are rejected (StorageFull);
               ///< older persisted events are kept.

  // --- Validation limits ---
  // String inputs must additionally be well-formed UTF-8; ill-formed input is
  // rejected by track()/start_session() rather than failing later inside the
  // background worker.
  std::size_t max_name_length = 64;  ///< Max event-name / property-key length.
  std::size_t max_properties = 64;   ///< Max properties per event.
  std::size_t max_string_length =
      1024;  ///< Max string-property length (bytes).
  std::size_t max_player_id_length =
      128;  ///< Max anonymous player-id length (bytes).

  // --- Callbacks (see logger.hpp for threading contract) ---
  LogCallback log_callback;
  ErrorCallback error_callback;
  PropertyFilter property_filter;
};

}  // namespace playertrace

#endif  // PLAYERTRACE_CONFIG_HPP
