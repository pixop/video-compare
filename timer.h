#pragma once

#include <cstdint>

class Timer {
	private:
		int64_t target_time;
		int64_t proportional;
		int64_t integral;
		int64_t derivative;

		const static int64_t P = 1;
		const static int64_t I = 0;
		const static int64_t D = 0;

	public:
		Timer();
		void wait(const int64_t period);
		void reset();

	private:
		int64_t adjust() const;
};
