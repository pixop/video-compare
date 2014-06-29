#pragma once

#include <cstdint>

class Timer {
private:
	int64_t target_time;
	int64_t proportional;
	int64_t integral;
	int64_t derivative;

	const static double P;
	const static double I;
	const static double D;

public:
	Timer();
	void wait(const int64_t period);
	void reset();

private:
	int64_t adjust() const;
};
