#include "side_aware_logger.h"
#include <iomanip>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "string_utils.h"

extern "C" {
#include <libavutil/log.h>
}

static std::recursive_mutex log_mutex;

thread_local Side log_side = NONE;

static std::unordered_map<Side, std::unordered_set<std::string>> ignored_log_messages_per_side;
static std::unordered_set<std::string> search_strings = {"No accelerated colorspace conversion found from"};

const char* to_string(Side side) {
  switch (side) {
    case LEFT:
      return "LEFT";
    case RIGHT:
      return "RIGHT";
    default:
      return "UNKNN";
  }
}

std::string sa_format_string(const Side side, const char* message) {
  std::string formatted_string;

  if (side > NONE) {
    if (message == nullptr) {
      formatted_string = string_sprintf("[%s]", to_string(side));
    } else {
      formatted_string = string_sprintf("[%s] %s", to_string(side), message);
    }
  } else {
    formatted_string = message;
  }

  return formatted_string;
}

void sa_av_log_callback(void* ptr, int level, const char* fmt, va_list args) {
  if (level > av_log_get_level()) {
    return;
  }

  const std::string message(fmt);

  // Check if the message starts with any of the substrings that generate log clutter
  bool is_noise = false;

  for (const auto& noise : search_strings) {
    if (message.find(noise) != std::string::npos) {
      is_noise = true;
      break;
    }
  }

  std::lock_guard<std::recursive_mutex> lock(log_mutex);

  // If noise and already logged, return early
  if (is_noise) {
    if (!ignored_log_messages_per_side[log_side].insert(message).second) {
      return;
    }
  }

  if (log_side > NONE) {
    std::cerr << std::setw(8) << std::left;
    std::cerr << sa_format_string(log_side, "");
    std::cerr << std::setw(0) << std::right;
  }

  av_log_default_callback(ptr, level, fmt, args);
}

void sa_invoke_av_log_callback(void* ptr, int level, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);

  sa_av_log_callback(ptr, level, fmt, args);

  va_end(args);
}

void sa_log(const Side side, int level, const std::string& message) {
  std::lock_guard<std::recursive_mutex> lock(log_mutex);
  ScopedLogSide scoped_log_side(side);

  sa_invoke_av_log_callback(nullptr, level, "%s\n", message.c_str());
}

void sa_log_info(const Side side, const std::string& message) {
  sa_log(side, AV_LOG_INFO, message);
}

void sa_log_warning(const Side side, const std::string& message) {
  sa_log(side, AV_LOG_WARNING, message);
}

void sa_log_error(const Side side, const std::string& message) {
  sa_log(side, AV_LOG_ERROR, message);
}
