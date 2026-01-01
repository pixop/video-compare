#pragma once
#include <array>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
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

  // Route SDL events to scope windows; windows with close requests are handled by reconcile()
  void route_events();

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

 private:
  const bool use_10_bpc_;
  const int display_number_;
  const ScopesConfig config_;

  std::array<std::unique_ptr<ScopeWindow>, ScopeWindow::kNumScopes> windows_;
  std::array<std::unique_ptr<WorkerState>, ScopeWindow::kNumScopes> workers_;
  std::array<uint64_t, ScopeWindow::kNumScopes> last_submitted_seq_{};
};


