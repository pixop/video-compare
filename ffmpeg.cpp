#include "ffmpeg.h"
#include <string>

namespace ffmpeg {
std::string error_string(const int error_code) {
  constexpr size_t size{128};
  std::array<char, size> buffer;
  av_make_error_string(buffer.data(), size, error_code);
  return std::string(buffer.data());
}

Error::Error(const std::string& message) : std::runtime_error{"FFmpeg: " + message} {}

Error::Error(const int status) : std::runtime_error{"FFmpeg: " + error_string(status)} {}

Error::Error(const std::string& file_name, int status) : std::runtime_error{file_name + ": " + error_string(status)} {}
}  // namespace ffmpeg
