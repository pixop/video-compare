#include "ffmpeg.h"

namespace ffmpeg {
std::string error_string(const int error_code) {
	constexpr size_t size{64};
	std::array<char, size> buffer;
	av_make_error_string(buffer.data(), size, error_code);
	return "FFmpeg: " + std::string(buffer.data());
}

Error::Error(const std::string &message) :
	std::runtime_error{"FFmpeg: " + message} {
}

Error::Error(const int status) :
	std::runtime_error{error_string(status)} {
}
}
