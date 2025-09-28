// RowWorkers — thread-reuse pool for row/stripe processing
//
// Features:
// - Reuses worker threads across frames.
// - Static even split OR dynamic chunking via atomic work-stealing.
// - Handles cases where total_rows < num_threads (excess workers sit out).

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class RowWorkers {
 public:
  explicit RowWorkers(int requested_threads = 0) {
    if (requested_threads <= 0) {
      const unsigned int hc = std::thread::hardware_concurrency();
      if (hc == 0U) {
        requested_threads = 1;
      } else {
        requested_threads = static_cast<int>(hc);
      }
    }

    num_threads_ = requested_threads;
    stop_requested_.store(false, std::memory_order_relaxed);
    job_available_.store(false, std::memory_order_relaxed);

    workers_.reserve(static_cast<std::size_t>(num_threads_));

    for (int i = 0; i < num_threads_; i++) {
      workers_.emplace_back([this, i]() { this->worker_loop(i); });
    }
  }

  ~RowWorkers() {
    {
      std::lock_guard<std::mutex> lk(mutex_);
      stop_requested_.store(true, std::memory_order_relaxed);
      cv_.notify_all();
    }

    for (std::size_t i = 0; i < workers_.size(); i++) {
      if (workers_[i].joinable()) {
        workers_[i].join();
      }
    }
  }

  int size() const { return num_threads_; }

  // Static even split of [0, total_rows)
  // func(start_row, end_row) must be thread-safe and write to disjoint outputs.
  void run_static(const int total_rows, const std::function<void(int, int)>& func) const {
    if (validate_and_setup_job(total_rows, false, 0, func, std::function<void(int, int, int)>(), false)) {
      execute_job();
    }
  }

  // Dynamic chunking: workers grab blocks of rows until done.
  // block_rows should usually be 64..512 for bandwidth-bound passes; tune.
  void run_dynamic(const int total_rows, const std::function<void(int, int)>& func, int block_rows) const {
    if (validate_and_setup_job(total_rows, true, block_rows, func, std::function<void(int, int, int)>(), false)) {
      execute_job();
    }
  }

  // Static even split with worker index passed to func(start,end,worker_idx)
  void run_static_indexed(const int total_rows, const std::function<void(int, int, int)>& func) const {
    if (validate_and_setup_job(total_rows, false, 0, std::function<void(int, int)>(), func, true)) {
      execute_job(true);
    }
  }

  // Dynamic chunking with worker index passed to func(start,end,worker_idx)
  void run_dynamic_indexed(const int total_rows, const std::function<void(int, int, int)>& func, int block_rows) const {
    if (validate_and_setup_job(total_rows, true, block_rows, std::function<void(int, int)>(), func, true)) {
      execute_job(true);
    }
  }

 private:
  int active_workers_locked() const {
    int rows = total_rows_;
    if (rows < 0) {
      rows = 0;
    }
    return std::min(num_threads_, rows);
  }

  bool validate_and_setup_job(const int total_rows, bool is_dynamic, int block_size, const std::function<void(int, int)>& func, const std::function<void(int, int, int)>& func_indexed, bool use_indexed) const {
    if (total_rows <= 0) {
      return false;
    }

    // Validate and set default block size for dynamic mode
    if (is_dynamic && block_size <= 0) {
      block_size = 64;
    }

    std::lock_guard<std::mutex> lk(mutex_);
    job_function_ = func;
    job_function_indexed_ = func_indexed;
    total_rows_ = total_rows;
    dynamic_mode_ = is_dynamic;
    dynamic_block_size_ = block_size;
    next_row_.store(0, std::memory_order_relaxed);
    workers_arrived_ = 0;
    workers_finished_ = 0;
    use_indexed_mode_ = use_indexed;
    job_available_.store(true, std::memory_order_release);
    return true;
  }

  void execute_job(bool reset_indexed_mode = false) const {
    cv_.notify_all();

    {
      std::unique_lock<std::mutex> lk(mutex_);

      done_cv_.wait(lk, [this]() { return workers_finished_ == active_workers_locked(); });
      job_available_.store(false, std::memory_order_relaxed);

      if (reset_indexed_mode) {
        use_indexed_mode_ = false;
      }
    }

    cv_.notify_all();
  }

  void worker_loop(const int worker_index) {
    for (;;) {
      // Wait for a job or stop signal
      int my_rank = -1;
      {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait(lk, [this]() { return stop_requested_.load(std::memory_order_relaxed) || job_available_.load(std::memory_order_acquire); });
        if (stop_requested_.load(std::memory_order_relaxed)) {
          return;
        }

        if (workers_arrived_ < active_workers_locked()) {
          my_rank = workers_arrived_++;  // rank in [0..active_workers_-1]
        } else {
          // Sit out this job; wait until job_available_ becomes false
          cv_.wait(lk, [this]() { return stop_requested_.load(std::memory_order_relaxed) || !job_available_.load(std::memory_order_acquire); });
          if (stop_requested_.load(std::memory_order_relaxed)) {
            return;
          }
          continue;
        }
      }

      // Take a snapshot of job state outside the lock to minimize lock time
      std::function<void(int, int)> job_function;
      std::function<void(int, int, int)> job_function_indexed;
      int total_rows;
      bool is_dynamic_mode;
      int block_size;
      bool use_indexed_mode;
      int active_workers;

      {
        std::lock_guard<std::mutex> lk(mutex_);
        job_function = job_function_;
        job_function_indexed = job_function_indexed_;
        total_rows = total_rows_;
        is_dynamic_mode = dynamic_mode_;
        block_size = dynamic_block_size_;
        use_indexed_mode = use_indexed_mode_;
        active_workers = active_workers_locked();
      }

      if (!is_dynamic_mode) {
        // Static mode: divide work evenly among active workers
        const int start_row = (my_rank * total_rows) / active_workers;
        const int end_row = ((my_rank + 1) * total_rows) / active_workers;

        if (start_row < end_row) {
          if (use_indexed_mode) {
            job_function_indexed(start_row, end_row, worker_index);
          } else {
            job_function(start_row, end_row);
          }
        }
      } else {
        // Dynamic mode: workers grab blocks of work until done
        for (;;) {
          const int start_row = next_row_.fetch_add(block_size, std::memory_order_relaxed);
          if (start_row >= total_rows) {
            break;
          }

          int end_row = start_row + block_size;
          if (end_row > total_rows) {
            end_row = total_rows;
          }

          if (use_indexed_mode) {
            job_function_indexed(start_row, end_row, worker_index);
          } else {
            job_function(start_row, end_row);
          }
        }
      }

      // Signal that this worker has finished
      {
        std::lock_guard<std::mutex> lk(mutex_);

        if (++workers_finished_ == active_workers) {
          job_available_.store(false, std::memory_order_relaxed);
          done_cv_.notify_one();
        }
      }
    }
  }

  int num_threads_ = 0;
  std::vector<std::thread> workers_;

  // Job state (mutable for const methods)
  mutable std::function<void(int, int)> job_function_;
  mutable std::function<void(int, int, int)> job_function_indexed_;
  mutable bool use_indexed_mode_ = false;
  mutable int total_rows_ = 0;
  mutable std::atomic<int> next_row_{0};
  mutable bool dynamic_mode_ = false;
  mutable int dynamic_block_size_ = 0;

  // Synchronization primitives
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  mutable std::condition_variable done_cv_;
  mutable std::atomic<bool> stop_requested_{false};
  mutable std::atomic<bool> job_available_{false};
  mutable int workers_arrived_ = 0;
  mutable int workers_finished_ = 0;
};

// Choose a reasonable block size for bandwidth-bound passes
inline int suggest_block_rows_by_bytes(const int image_width, const int image_height, const int bytes_per_pixel, const int channels = 3, long long target_bytes_per_chunk = 0LL) {
  // Choose rows so that rows * image_width * bytes_per_pixel * channels ~= target_bytes_per_chunk.
  if (image_width <= 0 || image_height <= 0 || bytes_per_pixel <= 0 || channels <= 0) {
    return 1;
  }

  // Default target ~256KB if caller passes <=0
  if (target_bytes_per_chunk <= 0) {
    target_bytes_per_chunk = 262144LL;  // 256 * 1024
  }

  const long long bytes_per_row = static_cast<long long>(image_width) * bytes_per_pixel * channels;
  long long rows_ll = target_bytes_per_chunk / bytes_per_row;
  if (rows_ll < 1LL) {
    rows_ll = 1LL;
  }

  // Clamp to a sane range; bandwidth-bound passes do well with 8..512 rows.
  const int min_rows = 8;
  const int max_rows = 512;

  long long clamped = rows_ll;
  if (clamped < static_cast<long long>(min_rows)) {
    clamped = static_cast<long long>(min_rows);
  }
  if (clamped > static_cast<long long>(max_rows)) {
    clamped = static_cast<long long>(max_rows);
  }

  // Also respect the image height.
  if (clamped > static_cast<long long>(image_height)) {
    clamped = static_cast<long long>(image_height);
  }

  // Optional: round to a small multiple for nicer load balancing.
  // Use multiple of 8, but never round down to zero.
  int rounded = static_cast<int>(clamped);
  const int multiple = 8;
  if (rounded > multiple) {
    const int rem = rounded % multiple;
    if (rem != 0) {
      rounded += (multiple - rem);
    }
    if (rounded > image_height) {
      rounded = image_height;
    }
  }

  if (rounded < 1) {
    rounded = 1;
  }

  return rounded;
}
