#pragma once
#include <array>
#include <stdexcept>
extern "C" {
	#include "libavutil/avutil.h"
}

namespace ffmpeg {
class Error : std::runtime_error {
public:
	Error(const std::string &message);
	Error(int status);
};

std::string error_string(const int error_code);

inline int check(const int status) {
	if (status < 0) {
		throw ffmpeg::Error{status};
	}
	return status;
}
}
