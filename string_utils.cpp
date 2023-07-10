#include "string_utils.h"
#include <numeric>
#include <cmath>
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

inline bool ci_compare_char(char a, char b) {
   return (::toupper(a) == b);
}

std::string::const_iterator string_ci_find(std::string& str, const std::string& query) {
   std::string tmp;

   std::transform(query.cbegin(), query.cend(), std::back_inserter(tmp), ::toupper);

   return (search(str.cbegin(), str.cend(), tmp.cbegin(), tmp.cend(), ci_compare_char));
}
