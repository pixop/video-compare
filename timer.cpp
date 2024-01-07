#include "timer.h"
#include <algorithm>
#include <iostream>
#include <thread>

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
  const auto lag = std::chrono::microseconds{period} + std::chrono::microseconds{adjust()};

  std::this_thread::sleep_for(lag);

  const int64_t error = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - target_time_).count();
  derivative_ = error - proportional_;
  integral_ += error;
  proportional_ = error;
}

int64_t Timer::adjust() const {
  return P * proportional_ + I * integral_ + D * derivative_;
}
