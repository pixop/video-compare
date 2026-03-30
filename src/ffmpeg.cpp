#include "ffmpeg.h"
#include <string>
#include "side_aware_logger.h"
#include "string_utils.h"

namespace ffmpeg {
std::string error_string(const int error_code) {
  constexpr size_t size{256};
  std::array<char, size> buffer;

  av_make_error_string(buffer.data(), size, error_code);

  return std::string(buffer.data());
}

Error::Error(const std::string& message) : std::runtime_error{sa_format_string(string_sprintf("FFmpeg: %s", message.c_str()))} {}

Error::Error(const int status) : std::runtime_error{sa_format_string(string_sprintf("FFmpeg: %s", error_string(status).c_str()))} {}

Error::Error(const std::string& file_name, int status) : std::runtime_error{sa_format_string(string_sprintf("%s: %s", file_name.c_str(), error_string(status).c_str()))} {}
}  // namespace ffmpeg
