#pragma once
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include "video_decoder.h"

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavutil/rational.h>
}

// Constexpr string utilities
namespace ConstexprStringUtils {
// constexpr strlen for char literals
constexpr size_t cstrlen(const char* s) {
  size_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  return n;
}

// Find the longest string length in an array of const char*
template <size_t N>
constexpr size_t longest_string_length(const char* const (&strings)[N]) {
  size_t max_len = 0;
  for (size_t i = 0; i < N; ++i) {
    const size_t len = cstrlen(strings[i]);
    if (len > max_len) {
      max_len = len;
    }
  }
  return max_len;
}
}  // namespace ConstexprStringUtils

// Credits to user2622016 for this C++11 approach
// https://stackoverflow.com/questions/2342162/stdstring-formatting-like-sprintf
template <typename... Args>
static std::string string_sprintf(const std::string& format, Args... args) {
  const int length = std::snprintf(nullptr, 0, format.c_str(), args...);
  assert(length >= 0);

  char* buf = new char[length + 1];
  std::snprintf(buf, length + 1, format.c_str(), args...);

  std::string str(buf);
  delete[] buf;
  return str;
}

std::string string_join(const std::vector<std::string>& strings, const std::string& delim);

std::vector<std::string> string_split(const std::string& str, char delim);

std::string format_position(const float position, const bool use_compact);

std::string format_duration(const float duration);

double parse_strict_double(const std::string& s);

double parse_timestamps_to_seconds(const std::string& timestamp);

std::string to_lower_case(const std::string& str);

std::string to_upper_case(const std::string& str);

std::string::const_iterator string_ci_find(std::string& str, const std::string& query);

std::string stringify_field_order(const AVFieldOrder field_order, const std::string& unknown = "") noexcept;

std::string stringify_frame_rate_only(const AVRational frame_rate) noexcept;

std::string stringify_frame_rate(const AVRational frame_rate, const AVFieldOrder field_order) noexcept;

std::string stringify_decoder(const VideoDecoder* video_decoder) noexcept;

std::string stringify_fraction(const uint64_t num, const uint64_t den, const unsigned precision);

std::string stringify_file_size(const int64_t size, const unsigned precision = 0) noexcept;

std::string stringify_bit_rate(const int64_t bit_rate, const unsigned precision = 0) noexcept;

std::string stringify_pixel_format(const AVPixelFormat pixel_format, const AVColorRange color_range, const AVColorSpace color_space, const AVColorPrimaries color_primaries, const AVColorTransferCharacteristic color_trc) noexcept;

void print_wrapped(const std::string& text, const size_t line_length);
