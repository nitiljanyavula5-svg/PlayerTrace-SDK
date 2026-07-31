// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Internal header — NOT installed and NOT part of the public API.
#ifndef PLAYERTRACE_INTERNAL_PIPELINE_HPP
#define PLAYERTRACE_INTERNAL_PIPELINE_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "internal/event_queue.hpp"
#include "internal/sqlite_store.hpp"
#include "internal/stats.hpp"
#include "playertrace/config.hpp"
#include "playertrace/consent.hpp"
#include "playertrace/event.hpp"
#include "playertrace/logger.hpp"
#include "playertrace/result.hpp"
#include "playertrace/sink.hpp"

namespace playertrace {
namespace internal {

/// Client lifecycle. Admission is open only in Running.
enum class LifecycleState { Running, Stopping, Stopped };

/// State co-owned by Client::Impl and the background worker thread.
///
/// It is held through a shared_ptr so the worker's thread body never
/// dereferences Client::Impl, which keeps the two lifetimes independent and the
/// locking simple. It is NOT a licence to abandon the thread: the worker is
/// always joined before the Client is destroyed, because it can reach state
/// captured by the user's sink and callbacks — state this object does not own
/// and cannot keep alive. See docs/reliability.md.
class Pipeline {
 public:
  Pipeline(Config config, std::unique_ptr<SqliteStore> store)
      : config_(std::move(config)),
        sink_(config_.sink),
        queue_(config_.max_queue_size, config_.batch_size),
        store_(std::move(store)),
        consent_(config_.consent) {}

  const Config& config() const noexcept { return config_; }
  const std::shared_ptr<Sink>& sink() const noexcept { return sink_; }
  EventQueue& queue() noexcept { return queue_; }
  Stats& stats() noexcept { return stats_; }
  const Stats& stats() const noexcept { return stats_; }

  // --- store access -------------------------------------------------------
  // Every store operation is serialized by this mutex so that the worker and a
  // synchronous consent purge cannot collide. Never hold it across a call into
  // user code (in particular Sink::write).
  std::mutex& store_mutex() noexcept { return store_mutex_; }
  SqliteStore& store() noexcept { return *store_; }

  // --- consent ------------------------------------------------------------
  ConsentState consent() const noexcept { return consent_.load(); }
  void set_consent_state(ConsentState s) noexcept { consent_.store(s); }
  std::uint64_t consent_generation() const noexcept {
    return consent_generation_.load();
  }
  /// Invalidates every event admitted so far. Called under the admission lock.
  void bump_consent_generation() noexcept { consent_generation_.fetch_add(1); }

  // --- durable capacity accounting ---------------------------------------
  // Events admitted but not yet delivered-and-acknowledged (or discarded).
  // Includes rows recovered from a previous run so the cap survives restarts.
  std::uint64_t outstanding() const noexcept { return outstanding_.load(); }
  void add_outstanding(std::uint64_t n) noexcept { outstanding_.fetch_add(n); }
  void release_outstanding(std::uint64_t n) noexcept {
    // Never wrap below zero; accounting errors must not disable the cap.
    std::uint64_t cur = outstanding_.load();
    while (true) {
      const std::uint64_t next = cur > n ? cur - n : 0;
      if (outstanding_.compare_exchange_weak(cur, next)) {
        return;
      }
    }
  }
  void reset_outstanding(std::uint64_t n) noexcept { outstanding_.store(n); }

  // --- terminal failure ----------------------------------------------------
  // Set exactly once, by the worker, when its loop exits for a reason other
  // than a stop request. Nothing will drain the queue after that, so admission
  // closes permanently: a producer must never be told Ok for work no thread
  // will ever process. It lives here, on state the admission path and the
  // worker already share, so neither needs a reference to the other.
  bool terminated() const noexcept { return terminated_.load(); }
  void set_terminated() noexcept { terminated_.store(true); }

  // --- callbacks ----------------------------------------------------------
  // Invoked from the worker thread, and from a caller thread for failures
  // raised there (Client::create, set_consent). Both contain any exception the
  // user's callback throws. See logger.hpp for the documented contract.
  void log(LogLevel level, const std::string& message) const {
    if (!config_.log_callback) {
      return;
    }
    try {
      config_.log_callback(level, message);
    } catch (...) {
      // A throwing log callback must never disturb the pipeline.
    }
  }

  void report_error(const Status& status) const {
    if (!config_.error_callback) {
      return;
    }
    try {
      config_.error_callback(status);
    } catch (...) {
      // A throwing error callback must never disturb the pipeline.
    }
  }

 private:
  Config config_;
  std::shared_ptr<Sink> sink_;
  EventQueue queue_;
  Stats stats_;

  std::mutex store_mutex_;
  std::unique_ptr<SqliteStore> store_;

  std::atomic<ConsentState> consent_;
  std::atomic<std::uint64_t> consent_generation_{0};
  std::atomic<std::uint64_t> outstanding_{0};
  std::atomic<bool> terminated_{false};
};

}  // namespace internal
}  // namespace playertrace

#endif  // PLAYERTRACE_INTERNAL_PIPELINE_HPP
