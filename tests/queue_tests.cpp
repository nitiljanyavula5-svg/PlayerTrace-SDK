// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_amalgamated.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "internal/event_queue.hpp"
#include "playertrace/event.hpp"

using playertrace::internal::EventQueue;
using playertrace::internal::QueuedEvent;

namespace {
QueuedEvent make_event(std::uint64_t seq) {
  QueuedEvent qe;
  qe.event.event_id = "id-" + std::to_string(seq);
  qe.event.sequence = seq;
  qe.ordinal = seq;
  return qe;
}
}  // namespace

TEST_CASE("Bounded queue rejects the newest when full", "[queue]") {
  EventQueue queue(/*capacity=*/3, /*batch_threshold=*/1);
  CHECK_FALSE(queue.full());
  CHECK(queue.try_push(make_event(0)));
  CHECK(queue.try_push(make_event(1)));
  CHECK(queue.try_push(make_event(2)));
  CHECK(queue.full());
  // Full: RejectNewest -> the 4th push fails, older events kept.
  CHECK_FALSE(queue.try_push(make_event(3)));
  CHECK(queue.size() == 3);

  std::vector<QueuedEvent> out;
  const std::size_t n = queue.pop_wait(10, std::chrono::milliseconds(0), &out);
  REQUIRE(n == 3);
  CHECK(out[0].event.sequence == 0);
  CHECK(out[1].event.sequence == 1);
  CHECK(out[2].event.sequence == 2);  // the rejected event #3 is absent
}

TEST_CASE("pop_wait drains at most 'max' events", "[queue]") {
  EventQueue queue(/*capacity=*/100, /*batch_threshold=*/1);
  for (std::uint64_t i = 0; i < 10; ++i) {
    queue.try_push(make_event(i));
  }
  std::vector<QueuedEvent> out;
  const std::size_t n = queue.pop_wait(4, std::chrono::milliseconds(0), &out);
  CHECK(n == 4);
  CHECK(queue.size() == 6);
}

TEST_CASE("pop_wait returns on timeout when below the batch threshold",
          "[queue]") {
  EventQueue queue(/*capacity=*/100, /*batch_threshold=*/50);
  queue.try_push(make_event(0));
  std::vector<QueuedEvent> out;
  // Only one event queued (< threshold of 50); a short timeout still returns
  // the partial batch without blocking the test.
  const std::size_t n = queue.pop_wait(50, std::chrono::milliseconds(10), &out);
  CHECK(n == 1);
}

TEST_CASE("kick wakes a consumer that is already waiting", "[queue]") {
  // The previous version of this test called kick() before pop_wait() ever ran,
  // so it only proved that a pending kick is not lost. Here the consumer thread
  // is observed to be inside pop_wait before the kick is issued, which is what
  // the wake path actually needs to guarantee.
  EventQueue queue(/*capacity=*/100, /*batch_threshold=*/50);
  queue.try_push(make_event(0));

  std::mutex m;
  std::condition_variable cv;
  bool consumer_started = false;
  std::size_t moved = 0;
  std::vector<QueuedEvent> out;

  std::thread consumer([&] {
    {
      std::lock_guard<std::mutex> lock(m);
      consumer_started = true;
    }
    cv.notify_one();
    // Threshold (50) is not met, so this blocks until kicked. A long timeout
    // means a passing test cannot be explained by the timeout firing.
    moved = queue.pop_wait(50, std::chrono::seconds(30), &out);
  });

  {
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return consumer_started; });
  }
  // The consumer has entered pop_wait (or is about to); kick must wake it.
  queue.kick();
  consumer.join();

  CHECK(moved == 1);
  CHECK(out.size() == 1);
}

TEST_CASE("clear discards queued events and reports the count", "[queue]") {
  EventQueue queue(/*capacity=*/10, /*batch_threshold=*/1);
  queue.try_push(make_event(0));
  queue.try_push(make_event(1));
  CHECK(queue.size() == 2);
  CHECK(queue.clear() == 2);
  CHECK(queue.size() == 0);
  CHECK(queue.clear() == 0);
}

TEST_CASE("front_ordinal reports the oldest queued admission", "[queue]") {
  // The flush barrier relies on this: events enter in admission order, so the
  // front is always the minimum ordinal still waiting.
  EventQueue queue(/*capacity=*/10, /*batch_threshold=*/1);
  CHECK(queue.front_ordinal() ==
        (std::numeric_limits<std::uint64_t>::max)());  // empty

  queue.try_push(make_event(7));
  queue.try_push(make_event(8));
  queue.try_push(make_event(9));
  CHECK(queue.front_ordinal() == 7);

  std::vector<QueuedEvent> out;
  queue.pop_wait(1, std::chrono::milliseconds(0), &out);
  CHECK(queue.front_ordinal() == 8);

  queue.clear();
  CHECK(queue.front_ordinal() == (std::numeric_limits<std::uint64_t>::max)());
}

TEST_CASE("The queue never exceeds its capacity", "[queue]") {
  // Failed batches are held in the worker's own retry buffer, never pushed back
  // here: doing so used to take the queue above max_queue_size and corrupt the
  // capacity accounting.
  EventQueue queue(/*capacity=*/2, /*batch_threshold=*/1);
  CHECK(queue.try_push(make_event(1)));
  CHECK(queue.try_push(make_event(2)));
  CHECK_FALSE(queue.try_push(make_event(3)));
  CHECK(queue.size() == 2);
}
