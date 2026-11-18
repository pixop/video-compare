#include "timer.h"
#include <algorithm>
#include <iostream>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
namespace {
struct WindowsHighResTimerScope {
  using TimePeriodFn = UINT(WINAPI*)(UINT);
  HMODULE winmm{nullptr};
  TimePeriodFn end_fn{nullptr};

  WindowsHighResTimerScope() {
    winmm = LoadLibraryA("winmm.dll");
    if (winmm != nullptr) {
      auto begin_fn = reinterpret_cast<TimePeriodFn>(reinterpret_cast<void*>(GetProcAddress(winmm, "timeBeginPeriod")));
      end_fn = reinterpret_cast<TimePeriodFn>(reinterpret_cast<void*>(GetProcAddress(winmm, "timeEndPeriod")));
      if (begin_fn != nullptr) {
        begin_fn(1);
      }
    }
  }
  ~WindowsHighResTimerScope() {
    if (end_fn != nullptr) {
      end_fn(1);
    }
    if (winmm != nullptr) {
      FreeLibrary(winmm);
    }
  }
};
static WindowsHighResTimerScope g_win_high_res_timer;
}  // namespace
#endif

Timer::Timer() {
  reset();
}

void Timer::reset() {
  proportional_ = 0;
  integral_ = 0;
  derivative_ = 0;

  update();
}

void Timer::update() {
  target_time_ = std::chrono::high_resolution_clock::now();
}

int64_t Timer::us_until_target() {
  return std::chrono::duration_cast<std::chrono::microseconds>(target_time_ - std::chrono::high_resolution_clock::now()).count();
}

void Timer::shift_target(int64_t period) {
  target_time_ += std::chrono::microseconds{period};
}

void Timer::wait(const int64_t period) {
  const int64_t adjusted_us = period + adjust();
  if (adjusted_us > 0) {
    const auto wake_deadline = std::chrono::high_resolution_clock::now() + std::chrono::microseconds{adjusted_us};
    // Sleep for the bulk of the interval, leaving the last ~1.5ms for a light yield loop
    // to avoid OS scheduler oversleep jitter.
    if (adjusted_us > 2000) {
      std::this_thread::sleep_for(std::chrono::microseconds{adjusted_us - 1500});
    }
    while (std::chrono::high_resolution_clock::now() < wake_deadline) {
      std::this_thread::yield();
    }
  }

  const int64_t error = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - target_time_).count();
  derivative_ = error - proportional_;
  integral_ += error;
  proportional_ = error;
}

int64_t Timer::adjust() const {
  return P * proportional_ + I * integral_ + D * derivative_;
}
