#include "timer.h"
#include <algorithm>
#include <thread>

Timer::Timer() :
	target_time_{std::chrono::high_resolution_clock::now()} {
}

void Timer::wait(const int64_t period) {
	target_time_ += std::chrono::microseconds{period};

	const auto lag =
		std::chrono::duration_cast<std::chrono::microseconds>(
			target_time_ - std::chrono::high_resolution_clock::now()) +
			std::chrono::microseconds{adjust()};

	std::this_thread::sleep_for(lag);

	const int64_t error =
		std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::high_resolution_clock::now() - target_time_).count();
	derivative_ = error - proportional_;
	integral_ += error;
	proportional_ = error;

}

void Timer::update() {
	target_time_ = std::chrono::high_resolution_clock::now();
}

int64_t Timer::adjust() const {
	return P_ * proportional_ + I_ * integral_ + D_ * derivative_;
}
