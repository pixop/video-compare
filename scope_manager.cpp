#include "scope_manager.h"
#include <algorithm>
#include <utility>
#include "scope_window.h"

static std::unique_ptr<ScopeWindow> make_scope_window(const ScopesConfig& config, bool use_10_bpc, int display_number, ScopeWindow::Type type) {
  return std::make_unique<ScopeWindow>(type,
                                       config.width / 2,
                                       config.height,
                                       config.always_on_top,
                                       display_number,
                                       use_10_bpc);
}

ScopeManager::ScopeManager(const ScopesConfig& config, const bool use_10_bpc, const int display_number)
    : use_10_bpc_(use_10_bpc), display_number_(display_number), config_(config) {
  auto maybe_add = [&](const bool enabled, const ScopeWindow::Type type) {
    if (!enabled) {
      return;
    }
    const size_t idx = ScopeWindow::index(type);
    create_window(idx);
    start_worker(idx);
  };

  maybe_add(config.histogram, ScopeWindow::Type::Histogram);
  maybe_add(config.vectorscope, ScopeWindow::Type::Vectorscope);
  maybe_add(config.waveform, ScopeWindow::Type::Waveform);
}

ScopeManager::~ScopeManager() {
  for (size_t idx = 0; idx < ScopeWindow::kNumScopes; ++idx) {
    stop_worker(idx);
    destroy_window(idx);
  }
}

void ScopeManager::route_events() {
  ScopeWindow::route_events(windows_);
}

bool ScopeManager::request_toggle(ScopeWindow::Type type) {
  const size_t idx = ScopeWindow::index(type);
  if (windows_[idx]) {
    // Close existing
    stop_worker(idx);
    destroy_window(idx);
    return false;
  } else {
    create_window(idx);
    start_worker(idx);
    return true;
  }
}

void ScopeManager::set_roi(const ScopeWindow::Roi& roi) {
  for (auto& window : windows_) {
    if (window) {
      window->set_roi(roi);
    }
  }
}

void ScopeManager::reconcile() {
  for (size_t idx = 0; idx < windows_.size(); ++idx) {
    auto& window = windows_[idx];
    auto& worker = workers_[idx];
    if (window && window->close_requested()) {
      stop_worker(idx);
      destroy_window(idx);
      continue;
    }
    if (window && !worker) {
      start_worker(idx);
    } else if (!window && worker) {
      stop_worker(idx);
    }
  }
}

void ScopeManager::submit_jobs(const AVFrame* left_frame, const AVFrame* right_frame) {
  for (size_t idx = 0; idx < windows_.size(); ++idx) {
    auto& window = windows_[idx];
    auto& worker = workers_[idx];
    if (window && worker) {
      std::lock_guard<std::mutex> lk(worker->mutex_);
      worker->left_frame_ = left_frame;
      worker->right_frame_ = right_frame;
      worker->job_seq_++;
      worker->has_job_ = true;
      last_submitted_seq_[idx] = worker->job_seq_;
      worker->cv_.notify_all();
    } else {
      last_submitted_seq_[idx] = 0;
    }
  }
}

void ScopeManager::wait_all() {
  for (size_t idx = 0; idx < workers_.size(); ++idx) {
    auto& worker = workers_[idx];
    const uint64_t target_seq = last_submitted_seq_[idx];
    if (worker && target_seq != 0) {
      std::unique_lock<std::mutex> lk(worker->mutex_);
      worker->cv_.wait(lk, [&]() { return worker->done_seq_ >= target_seq || worker->stop_; });
    }
  }
}

void ScopeManager::render_all() {
  for (auto& window : windows_) {
    if (window) {
      window->render();
    }
  }
}

bool ScopeManager::has_any() const {
  for (const auto& window : windows_) {
    if (window) {
      return true;
    }
  }
  return false;
}

void ScopeManager::create_window(const size_t idx) {
  windows_[idx] = make_scope_window(config_, use_10_bpc_, display_number_, ScopeWindow::type_for_index(idx));
}

void ScopeManager::destroy_window(const size_t idx) {
  windows_[idx].reset();
}

void ScopeManager::start_worker(const size_t idx) {
  if (workers_[idx]) {
    return;
  }
  workers_[idx] = std::make_unique<WorkerState>();
  auto* worker_state = workers_[idx].get();
  worker_state->thread_ = std::thread([this, idx, worker_state]() {
    while (true) {
      const AVFrame* left_frame_local = nullptr;
      const AVFrame* right_frame_local = nullptr;
      uint64_t seq = 0;
      {
        std::unique_lock<std::mutex> lk(worker_state->mutex_);
        worker_state->cv_.wait(lk, [&]() { return worker_state->has_job_ || worker_state->stop_; });
        if (worker_state->stop_) {
          break;
        }
        left_frame_local = worker_state->left_frame_;
        right_frame_local = worker_state->right_frame_;
        seq = worker_state->job_seq_;
        worker_state->has_job_ = false;
      }
      auto& win = windows_[idx];
      if (win) {
        win->prepare(left_frame_local, right_frame_local);
      }
      {
        std::lock_guard<std::mutex> lk(worker_state->mutex_);
        worker_state->done_seq_ = seq;
      }
      worker_state->cv_.notify_all();
    }
  });
}

void ScopeManager::stop_worker(const size_t idx) {
  auto& worker_state_ptr = workers_[idx];
  if (!worker_state_ptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lk(worker_state_ptr->mutex_);
    worker_state_ptr->stop_ = true;
    worker_state_ptr->cv_.notify_all();
  }
  if (worker_state_ptr->thread_.joinable()) {
    worker_state_ptr->thread_.join();
  }
  workers_[idx].reset();
}


