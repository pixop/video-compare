#include "timer.h"

#include <algorithm>

extern "C" {
	#include <libavutil/time.h>
}

using std::max;

Timer::Timer() :
		target_time(av_gettime()),
		proportional(0),
		integral(0),
		derivative(0) {
}

const double Timer::P = 0.0;
const double Timer::I = -1.0;
const double Timer::D = 0.0;

void Timer::wait(const int64_t period) {
	target_time += period;

	int64_t lag = target_time - av_gettime();
	lag += adjust();

	if (lag > 0) {
		av_usleep(static_cast<unsigned>(lag));
	}

	int64_t error = av_gettime() - target_time;
	derivative = error - proportional;
	integral += error;
	proportional = error;

}

void Timer::reset() {
	target_time = av_gettime();
}

int64_t Timer::adjust() const {
	return P * proportional + I * integral + D * derivative;
}
