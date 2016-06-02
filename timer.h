#pragma once
#include <cstdint>
#include <chrono>

class Timer {
private:
	std::chrono::time_point<std::chrono::high_resolution_clock> target_time_;

	int64_t proportional_{};
	int64_t integral_{};
	int64_t derivative_{};

	constexpr static double P_{0.0};
	constexpr static double I_{-1.0};
	constexpr static double D_{0.0};

public:
	Timer();
	void wait(int64_t period);
	void update();

private:
	int64_t adjust() const;
};
