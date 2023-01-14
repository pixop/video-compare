#pragma once
#include <string>
#include <vector>
#include <cassert>

// Credits to user2622016 for this C++11 approach
// https://stackoverflow.com/questions/2342162/stdstring-formatting-like-sprintf
template<typename... Args>
static std::string string_sprintf(const char *format, Args... args) {
  const int length = std::snprintf(nullptr, 0, format, args...);
  assert(length >= 0);

  char* buf = new char[length + 1];
  std::snprintf(buf, length + 1, format, args...);

  std::string str(buf);
  delete[] buf;
  return str;
}

std::string string_join(std::vector<std::string> &strings, const std::string delim);
