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

void Timer::wait(const int64_t period) {
	target_time += period;

	int64_t lag = target_time - av_gettime();
	if (lag) {

		lag += adjust();

		av_usleep(max(0u, static_cast<unsigned>(lag)));
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
