#pragma once
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavutil/rational.h>
}

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

std::string string_join(std::vector<std::string>& strings, const std::string& delim);

std::string format_position(const float position, const bool use_compact);

std::string::const_iterator string_ci_find(std::string& str, const std::string& query);

std::string stringify_frame_rate(const AVRational frame_rate, const AVFieldOrder field_order) noexcept;

std::string stringify_fraction(const uint64_t num, const uint64_t den, const unsigned precision);

std::string stringify_file_size(const int64_t size, const unsigned precision = 0) noexcept;

std::string stringify_bit_rate(const int64_t bit_rate, const unsigned precision = 0) noexcept;
