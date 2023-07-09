#include "string_utils.h"
#include <numeric>
#include <algorithm>

// Borrowed from https://www.techiedelight.com/implode-a-vector-of-strings-into-a-comma-separated-string-in-cpp/
std::string string_join(std::vector<std::string>& strings, const std::string& delim) {
  return std::accumulate(strings.begin(), strings.end(), std::string(), [&delim](std::string& x, std::string& y) { return x.empty() ? y : x + delim + y; });
}

std::string format_position(const float position, const bool use_compact) {
  const float rounded_millis = std::round(position * 1000.0F);

  const int milliseconds = rounded_millis;
  const int seconds = milliseconds / 1000;
  const int minutes = seconds / 60;
  const int hours = minutes / 60;

  if (!use_compact || minutes >= 60) {
    return string_sprintf("%02d:%02d:%02d.%03d", hours, minutes % 60, seconds % 60, milliseconds % 1000);
  }
  if (seconds >= 60) {
    return string_sprintf("%02d:%02d.%03d", minutes, seconds % 60, milliseconds % 1000);
  }

  return string_sprintf("%d.%03d", seconds, milliseconds % 1000);
}