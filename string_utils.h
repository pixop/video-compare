#pragma once
#include <cassert>
#include <string>
#include <vector>

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

std::string::const_iterator string_ci_find(std::string& s, const std::string& p);