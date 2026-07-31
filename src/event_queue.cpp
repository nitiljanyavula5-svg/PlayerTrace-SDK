// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#include "internal/event_queue.hpp"

#include <limits>
#include <utility>

namespace playertrace {
namespace internal {

bool EventQueue::full() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size() >= capacity_;
}

bool EventQueue::has_room_for(std::size_t count) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::size_t used =
      queue_.size() < capacity_ ? queue_.size() : capacity_;
  return capacity_ - used >= count;
}

bool EventQueue::try_push(QueuedEvent event) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= capacity_) {
      return false;  // RejectNewest: drop the incoming event.
    }
    queue_.push_back(std::move(event));
  }
  cv_.notify_one();
  return true;
}

std::uint64_t EventQueue::front_ordinal() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.empty()) {
    return (std::numeric_limits<std::uint64_t>::max)();
  }
  return queue_.front().ordinal;
}

std::size_t EventQueue::pop_wait(std::size_t max,
                                 std::chrono::milliseconds timeout,
                                 std::vector<QueuedEvent>* out) {
  std::unique_lock<std::mutex> lock(mutex_);
  // Wake when a full batch is ready or we were kicked; otherwise the timeout
  // (batch_interval) fires and we process whatever partial batch is present.
  // With batch_threshold_ == 1 this reduces to "wake on any queued event".
  cv_.wait_for(lock, timeout,
               [this] { return queue_.size() >= batch_threshold_ || kicked_; });
  kicked_ = false;

  std::size_t moved = 0;
  while (!queue_.empty() && moved < max) {
    out->push_back(std::move(queue_.front()));
    queue_.pop_front();
    ++moved;
  }
  return moved;
}

void EventQueue::kick() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    kicked_ = true;
  }
  cv_.notify_all();
}

std::size_t EventQueue::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::size_t dropped = queue_.size();
  queue_.clear();
  return dropped;
}

std::size_t EventQueue::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

}  // namespace internal
}  // namespace playertrace
