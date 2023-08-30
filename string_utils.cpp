#include "string_utils.h"
#include <algorithm>
#include <cmath>
#include <numeric>

// Borrowed from https://www.techiedelight.com/implode-a-vector-of-strings-into-a-comma-separated-string-in-cpp/
std::string string_join(std::vector<std::string>& strings, const std::string& delim) {
  return std::accumulate(strings.begin(), strings.end(), std::string(), [&delim](std::string& x, std::string& y) { return x.empty() ? y : x + delim + y; });
}

std::string format_position(const float position, const bool use_compact) {
  const std::string sign = position < 0 ? "-" : "";
  const float rounded_millis = std::round(std::fabs(position) * 1000.0F);

  const int milliseconds = rounded_millis;
  const int seconds = milliseconds / 1000;
  const int minutes = seconds / 60;
  const int hours = minutes / 60;

  if (!use_compact || minutes >= 60) {
    return string_sprintf("%s%02d:%02d:%02d.%03d", sign.c_str(), hours, minutes % 60, seconds % 60, milliseconds % 1000);
  }
  if (seconds >= 60) {
    return string_sprintf("%s%02d:%02d.%03d", sign.c_str(), minutes, seconds % 60, milliseconds % 1000);
  }

  return string_sprintf("%s%d.%03d", sign.c_str(), seconds, milliseconds % 1000);
}

inline bool ci_compare_char(char a, char b) {
  return (::toupper(a) == b);
}

std::string::const_iterator string_ci_find(std::string& str, const std::string& query) {
  std::string tmp;

  std::transform(query.cbegin(), query.cend(), std::back_inserter(tmp), ::toupper);

  return (search(str.cbegin(), str.cend(), tmp.cbegin(), tmp.cend(), ci_compare_char));
}

// Slightly modified from https://stackoverflow.com/questions/63511627/how-can-i-stringify-a-fraction-with-n-decimals-in-c/63511628#63511628
std::string stringify_fraction(const uint64_t num, const uint64_t den, const unsigned precision) {
  constexpr unsigned base = 10;

  // prevent division by zero if necessary
  if (den == 0) {
    return "inf";
  }

  // integral part can be computed using regular division
  std::string result = std::to_string(num / den);

  // perform first step of long division
  // also cancel early if there is no fractional part
  uint64_t tmp = num % den;
  if (tmp == 0 || precision == 0) {
    return result;
  }

  // reserve characters to avoid unnecessary re-allocation
  result.reserve(result.size() + precision + 1);

  // fractional part can be computed using long divison
  result += '.';
  for (size_t i = 0; i < precision; ++i) {
    tmp *= base;
    char nextDigit = '0' + static_cast<char>(tmp / den);
    result.push_back(nextDigit);
    tmp %= den;
  }

  return result;
}

static const uint64_t POWERS_OF_1000[] = {1, 1000, 1000000, 1000000000, 1000000000000, 1000000000000000, 1000000000000000000};
static const uint64_t POWERS_OF_1024[] = {1, 1024, 1048576, 1073741824, 1099511627776, 1125899906842624, 1152921504606846976};

int uint64_log(uint64_t n, const uint64_t* power_table, const size_t table_size) {
  int left = 0;
  int right = table_size / sizeof(uint64_t) - 1;

  // binary search
  while (left < right) {
    int mid = (left + right + 1) / 2;

    if (power_table[mid] <= n) {
      left = mid;
    } else {
      right = mid - 1;
    }
  }

  return left;
}

static const char FILE_SIZE_UNITS[7][3] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};

// Derived from https://stackoverflow.com/questions/63512258/how-can-i-print-a-human-readable-file-size-in-c-without-a-loop
std::string stringify_file_size(const int64_t size, const unsigned precision) noexcept {
  if (size < 0) {
    return "unknown size";
  }

  unsigned unit = uint64_log(size, POWERS_OF_1024, sizeof(POWERS_OF_1024));

  std::string result = stringify_fraction(size, POWERS_OF_1024[unit], precision);
  result.reserve(result.size() + 5);

  result.push_back(' ');
  result.push_back(FILE_SIZE_UNITS[unit][0]);

  if (unit != 0) {
    result.push_back('i');
    result.push_back(FILE_SIZE_UNITS[unit][1]);
  }

  return result;
}

// Derived from https://stackoverflow.com/questions/63512258/how-can-i-print-a-human-readable-file-size-in-c-without-a-loop
std::string stringify_bit_rate(const int64_t bit_rate, const unsigned precision) noexcept {
  if (bit_rate == 0) {
    return "unknown bitrate";
  }

  unsigned unit = uint64_log(bit_rate, POWERS_OF_1000, sizeof(POWERS_OF_1000));

  std::string result = stringify_fraction(bit_rate, POWERS_OF_1000[unit], precision);
  result.reserve(result.size() + 8);

  result.push_back(' ');

  if (unit != 0) {
    char first = FILE_SIZE_UNITS[unit][0];
    first += 'a' - 'A';
    result.push_back(first);
  }

  result.append("b/s");

  return result;
}

std::string stringify_frame_rate(const AVRational frame_rate) noexcept {
  static const std::string postfix = "fps";
  const double d = av_q2d(frame_rate);

  // formatting code borrowed (with love!) from libavformat/dump.c
  const uint64_t v = lrintf(av_q2d(frame_rate) * 100);

  if (!v) {
    return string_sprintf("%1.4f %s", d, postfix.c_str());
  } else if (v % 100) {
    return string_sprintf("%3.2f %s", d, postfix.c_str());
  } else if (v % (100 * 1000)) {
    return string_sprintf("%1.0f %s", d, postfix.c_str());
  }

  return string_sprintf("%1.0fk %s", d / 1000, postfix.c_str());
}
