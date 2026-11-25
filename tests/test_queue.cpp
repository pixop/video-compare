#include "queue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void expect(const bool condition, const char* description) {
  if (!condition) {
    throw std::runtime_error(description);
  }
}

template <typename Result>
class AsyncQueueOperation {
 public:
  template <typename Operation>
  AsyncQueueOperation(Queue<int>& queue, Operation operation)
      : queue_(queue), result_(promise_.get_future()), started_result_(started_.get_future()), worker_([this, operation]() {
          started_.set_value();
          promise_.set_value(operation());
        }) {}

  AsyncQueueOperation(const AsyncQueueOperation&) = delete;
  AsyncQueueOperation& operator=(const AsyncQueueOperation&) = delete;

  ~AsyncQueueOperation() {
    // Also unblock the worker if an assertion fails before its normal wakeup.
    queue_.quit();
    worker_.join();
  }

  void expect_waiting(const char* description) {
    expect(started_result_.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "worker did not start");
    expect(result_.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout, description);
  }

  Result get(const char* description) {
    expect(result_.wait_for(std::chrono::seconds(5)) == std::future_status::ready, description);
    return result_.get();
  }

 private:
  Queue<int>& queue_;
  std::promise<Result> promise_;
  std::future<Result> result_;
  std::promise<void> started_;
  std::future<void> started_result_;
  std::thread worker_;
};

void test_fifo_stop_drain_and_restart() {
  Queue<int> queue(2);
  expect(queue.push(11) && queue.push(22), "initial pushes succeed");
  expect(queue.size() == 2, "queue preserves capacity");

  queue.stop();
  expect(queue.is_stopped(), "stop sets stopped state");
  expect(!queue.push(33), "stopped queue rejects pushes");
  int item = 0;
  expect(queue.pop(item) && item == 11, "stop preserves first queued item");
  expect(queue.pop(item) && item == 22, "stop preserves second queued item");
  expect(!queue.pop(item), "drained stopped queue ends reads");

  queue.restart();
  expect(!queue.is_stopped(), "restart clears stopped state");
  expect(queue.push(44), "restart permits pushes");
  expect(queue.pop(item) && item == 44, "restart permits reads");
}

void test_waiting_consumer_receives_item() {
  Queue<int> queue(1);
  AsyncQueueOperation<std::pair<bool, int>> consumer(queue, [&]() {
    int item = -1;
    const bool received = queue.pop(item);
    return std::make_pair(received, item);
  });
  consumer.expect_waiting("consumer waits on empty queue");
  expect(queue.push(7), "push wakes waiting consumer");
  const auto result = consumer.get("consumer did not wake after push");
  expect(result.first && result.second == 7, "consumer receives pushed item");
}

void test_stop_wakes_empty_consumer() {
  Queue<int> queue(1);
  AsyncQueueOperation<bool> consumer(queue, [&]() {
    int item = 0;
    return queue.pop(item);
  });
  consumer.expect_waiting("consumer waits before stop");
  queue.stop();
  expect(!consumer.get("stop did not wake empty consumer"), "stopped empty queue returns false");
}

void test_pop_wakes_full_producer() {
  Queue<int> queue(1);
  expect(queue.push(1), "fill queue");
  AsyncQueueOperation<bool> producer(queue, [&]() { return queue.push(2); });
  producer.expect_waiting("producer waits on full queue");
  int item = 0;
  expect(queue.pop(item) && item == 1, "pop frees space");
  expect(producer.get("pop did not wake full producer"), "producer succeeds after pop");
  expect(queue.pop(item) && item == 2, "new item is stored correctly");
}

void test_stop_then_empty_wakes_full_producer() {
  Queue<int> queue(1);
  expect(queue.push(1), "fill queue before seek-like stop");
  AsyncQueueOperation<bool> producer(queue, [&]() { return queue.push(2); });
  producer.expect_waiting("producer waits before seek-like stop");
  // This is video-compare's existing seek protocol; no changes to Queue::stop().
  queue.stop();
  queue.empty();
  expect(!producer.get("stop + empty did not unblock producer"), "stopped producer does not enqueue stale data");
  expect(queue.is_empty(), "seek-like stop drains stale data");
  queue.restart();
  int item = 0;
  expect(queue.push(3) && queue.pop(item) && item == 3, "queue recovers after seek-like restart");
}

void test_quit_wakes_full_producer() {
  Queue<int> queue(1);
  expect(queue.push(1), "fill queue before quit");
  AsyncQueueOperation<bool> producer(queue, [&]() { return queue.push(2); });
  producer.expect_waiting("producer waits before quit");
  queue.quit();
  expect(!producer.get("quit did not unblock producer"), "quit aborts pending push");
}

void test_quit_wakes_empty_consumer() {
  Queue<int> queue(1);
  AsyncQueueOperation<bool> consumer(queue, [&]() {
    int item = 0;
    return queue.pop(item);
  });
  consumer.expect_waiting("consumer waits before quit");
  queue.quit();
  expect(!consumer.get("quit did not unblock consumer"), "quit aborts pending pop");
}


void test_concurrent_producers_and_consumers() {
  constexpr int kProducers = 3;
  constexpr int kConsumers = 4;
  constexpr int kItemsPerProducer = 400;
  constexpr int kTotal = kProducers * kItemsPerProducer;
  Queue<int> queue(8);
  std::vector<std::atomic<int>> seen(kTotal);
  for (auto& count : seen) {
    count.store(0);
  }
  std::atomic<int> invalid_items{0};
  std::atomic<int> failed_pushes{0};
  std::vector<std::thread> consumers;
  for (int i = 0; i < kConsumers; ++i) {
    consumers.emplace_back([&]() {
      int item = 0;
      while (queue.pop(item)) {
        if (item >= 0 && item < kTotal) {
          seen[item].fetch_add(1);
        } else {
          invalid_items.fetch_add(1);
        }
      }
    });
  }
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p]() {
      for (int i = 0; i < kItemsPerProducer; ++i) {
        if (!queue.push(p * kItemsPerProducer + i)) {
          failed_pushes.fetch_add(1);
        }
      }
    });
  }
  for (auto& producer : producers) {
    producer.join();
  }
  queue.stop();
  for (auto& consumer : consumers) {
    consumer.join();
  }
  expect(failed_pushes.load() == 0, "no pushes fail while queue is running");
  expect(invalid_items.load() == 0, "no invalid item values delivered");
  for (const auto& count : seen) {
    expect(count.load() == 1, "each item delivered exactly once");
  }
  expect(queue.is_empty(), "concurrent consumers fully drain queue");
}

}  // namespace

int main() {
  try {
    test_fifo_stop_drain_and_restart();
    test_waiting_consumer_receives_item();
    test_stop_wakes_empty_consumer();
    test_pop_wakes_full_producer();
    test_stop_then_empty_wakes_full_producer();
    test_quit_wakes_full_producer();
    test_quit_wakes_empty_consumer();
    test_concurrent_producers_and_consumers();
    std::puts("PASS queue unit tests");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL queue unit tests: %s\n", e.what());
    return 1;
  }
}
