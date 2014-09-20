#pragma once

#include <cstdint>

class Timer {
private:
	int64_t target_time;
	int64_t proportional {};
	int64_t integral {};
	int64_t derivative {};

	constexpr static double P {0.0};
	constexpr static double I {-1.0};
	constexpr static double D {0.0};

public:
	Timer();
	void wait(int64_t period);
	void reset();

private:
	int64_t adjust() const;
};
