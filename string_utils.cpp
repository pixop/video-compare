#include "string_utils.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <sstream>

extern "C" {
#include <libavutil/pixdesc.h>
}

// Borrowed from https://www.techiedelight.com/implode-a-vector-of-strings-into-a-comma-separated-string-in-cpp/
std::string string_join(const std::vector<std::string>& strings, const std::string& delim) {
  return std::accumulate(strings.begin(), strings.end(), std::string(), [&delim](const std::string& x, const std::string& y) { return x.empty() ? y : x + delim + y; });
}

std::vector<std::string> string_split(const std::string& string, char delim) {
  std::vector<std::string> tokens;
  std::string token;
  std::istringstream token_stream(string);

  while (std::getline(token_stream, token, delim)) {
    tokens.push_back(token);
  }

  return tokens;
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

std::string format_duration(const float duration) {
  return duration > 0 ? format_position(duration, false) : "unknown duration";
}

double parse_strict_double(const std::string& s) {
  if (s.empty()) {
    throw std::invalid_argument("Empty string is not a valid float");
  }

  char* end = nullptr;
  const char* str = s.c_str();
  double val = std::strtod(str, &end);

  if (end != (str + s.size()) || !std::isfinite(val)) {
    throw std::invalid_argument("Invalid floating point string: " + s);
  }

  return val;
}

double parse_timestamps_to_seconds(const std::string& timestamp) {
  std::istringstream ss(timestamp);
  std::string token;
  std::vector<std::string> parts;

  // split the timestamp by ':'
  while (std::getline(ss, token, ':')) {
    parts.push_back(token);
  }

  if (parts.empty() || parts.size() > 3) {
    throw std::invalid_argument("Invalid timestamp format");
  }

  // Initialize time components
  int hours = 0, minutes = 0;
  double seconds = 0.0;

  try {
    if (parts.size() == 3) {
      hours = std::stoi(parts[0]);
      minutes = std::stoi(parts[1]);
      seconds = parse_strict_double(parts[2]);
    } else if (parts.size() == 2) {
      minutes = std::stoi(parts[0]);
      seconds = parse_strict_double(parts[1]);
    } else if (parts.size() == 1) {
      seconds = parse_strict_double(parts[0]);
    }
  } catch (const std::exception& e) {
    throw std::invalid_argument("Invalid numeric value in timestamp");
  }

  return hours * 3600.0 + minutes * 60.0 + seconds;
}

std::string to_lower_case(const std::string& str) {
  std::string tmp;

  std::transform(str.begin(), str.end(), std::back_inserter(tmp), ::tolower);

  return tmp;
}

std::string to_upper_case(const std::string& str) {
  std::string tmp;

  std::transform(str.begin(), str.end(), std::back_inserter(tmp), ::toupper);

  return tmp;
}

inline bool ci_compare_char(char a, char b) {
  return (::tolower(a) == b);
}

std::string::const_iterator string_ci_find(std::string& str, const std::string& query) {
  const std::string lower_case_query = to_lower_case(query);

  return (search(str.cbegin(), str.cend(), lower_case_query.cbegin(), lower_case_query.cend(), ci_compare_char));
}

std::string stringify_field_order(const AVFieldOrder field_order, const std::string& unknown) noexcept {
  switch (field_order) {
    case AV_FIELD_PROGRESSIVE:
      return "progressive";
    case AV_FIELD_TT:
      return "top first";
    case AV_FIELD_BB:
      return "bottom first";
    case AV_FIELD_TB:
      return "top coded first, swapped";
    case AV_FIELD_BT:
      return "bottom coded first, swapped";
    default:
      return unknown;
  }
}

std::string stringify_frame_rate_only(const AVRational frame_rate) noexcept {
  static const std::string postfix = "fps";

  // formatting code borrowed (with love!) from libavformat/dump.c
  const double d = av_q2d(frame_rate);
  const uint64_t v = lrintf(d * 100);

  if (!v) {
    return string_sprintf("%1.4f %s", d, postfix.c_str());
  } else if (v % 100) {
    return string_sprintf("%3.2f %s", d, postfix.c_str());
  } else if (v % (100 * 1000)) {
    return string_sprintf("%1.0f %s", d, postfix.c_str());
  }

  return string_sprintf("%1.0fk %s", d / 1000, postfix.c_str());
}

std::string stringify_frame_rate(const AVRational frame_rate, const AVFieldOrder field_order) noexcept {
  const auto field_order_str = stringify_field_order(field_order);

  return stringify_frame_rate_only(frame_rate) + (field_order_str.empty() ? "" : " (" + field_order_str + ")");
}

std::string stringify_decoder(const VideoDecoder* video_decoder) noexcept {
  return video_decoder->is_hw_accelerated() ? string_sprintf("%s (%s)", video_decoder->codec()->name, video_decoder->hw_accel_name().c_str()) : video_decoder->codec()->name;
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

std::string stringify_pixel_format(const AVPixelFormat pixel_format, const AVColorRange color_range, const AVColorSpace color_space, const AVColorPrimaries color_primaries, const AVColorTransferCharacteristic color_trc) noexcept {
  std::string color_range_str;
  std::string color_space_str;

  // code adapted from FFmpeg's avcodec.c (thanks guys!)
  auto unknown_if_null = [](const char* str) { return str ? str : "unknown"; };

  if (color_range == AVCOL_RANGE_UNSPECIFIED || (color_range_str = av_color_range_name(color_range)).empty()) {
    color_range_str = "";
  }

  if (color_space != AVCOL_SPC_UNSPECIFIED || color_primaries != AVCOL_PRI_UNSPECIFIED || color_trc != AVCOL_TRC_UNSPECIFIED) {
    const char* col = unknown_if_null(av_color_space_name(color_space));
    const char* pri = unknown_if_null(av_color_primaries_name(color_primaries));
    const char* trc = unknown_if_null(av_color_transfer_name(color_trc));

    if (strcmp(col, pri) || strcmp(col, trc)) {
      color_space_str = string_sprintf("%s/%s/%s", col, pri, trc);
    } else {
      color_space_str = col;
    }
  } else {
    color_space_str = "";
  }

  const std::string range_color_space_separator = !color_range_str.empty() && !color_space_str.empty() ? ", " : "";
  const std::string range_and_color_space = !color_range_str.empty() || !color_space_str.empty() ? string_sprintf(" (%s%s%s)", color_range_str.c_str(), range_color_space_separator.c_str(), color_space_str.c_str()) : "";

  return string_sprintf("%s%s", av_get_pix_fmt_name(pixel_format), range_and_color_space.c_str());
}

void print_wrapped(const std::string& text, const size_t line_length) {
  size_t pos = 0;

  while (pos < text.length()) {
    size_t next_pos = (pos + line_length < text.length()) ? pos + line_length : text.length();

    if (next_pos != text.length()) {
      size_t space_pos = text.rfind(' ', next_pos);

      if (space_pos != std::string::npos && space_pos > pos) {
        next_pos = space_pos;
      }
    }

    std::cout << text.substr(pos, next_pos - pos) << std::endl;

    pos = text.find_first_not_of(' ', next_pos);

    if (pos == std::string::npos) {
      break;
    }
  }
}
