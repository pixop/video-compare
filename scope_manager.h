#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "config.h"
#include "scope_window.h"
extern "C" {
#include <libavutil/frame.h>
}

class ScopeManager {
 public:
  ScopeManager(const ScopesConfig& config, bool use_10_bpc, int display_number);
  ~ScopeManager();

  // Dispatch a single SDL event to scope windows; returns true if consumed.
  // Windows that request close are destroyed later by reconcile() at a safe sync point.
  bool handle_event(const SDL_Event& event);

  // Request toggle; returns true if a window was opened (so caller can refocus main window)
  bool request_toggle(ScopeWindow::Type type);

  // Update ROI for all open scope windows
  void set_roi(const ScopeWindow::Roi& roi);

  // Start/stop workers to match windows and destroy windows that requested close
  void reconcile();

  // Submit current pair of frames to all active scope workers
  void submit_jobs(const AVFrame* left_frame, const AVFrame* right_frame);
  // Wait for all submitted jobs to complete
  void wait_all();
  // Render all open scope windows (main thread)
  void render_all();

  // Returns true if any scope window is open
  bool has_any() const;

  // Fatal error handling for scope worker threads (e.g. bad filter options causing avfilter parse/config failures)
  bool has_fatal_error() const { return fatal_error_.load(std::memory_order_relaxed); }
  std::string fatal_error_message() const;

 private:
  struct WorkerState {
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    const AVFrame* left_frame_{nullptr};
    const AVFrame* right_frame_{nullptr};
    bool has_job_{false};
    bool stop_{false};
    uint64_t job_seq_{0};
    uint64_t done_seq_{0};
  };

  void create_window(size_t idx);
  void destroy_window(size_t idx);
  void start_worker(size_t idx);
  void stop_worker(size_t idx);
  void set_fatal_error(const std::string& message);

 private:
  const bool use_10_bpc_;
  const int display_number_;
  const ScopesConfig config_;

  std::array<std::unique_ptr<ScopeWindow>, ScopeWindow::kNumScopes> windows_;
  std::array<std::unique_ptr<WorkerState>, ScopeWindow::kNumScopes> workers_;
  std::array<uint64_t, ScopeWindow::kNumScopes> last_submitted_seq_{};

  std::atomic_bool fatal_error_{false};
  mutable std::mutex fatal_error_mutex_;
  std::string fatal_error_message_;
};
