#include "side_aware_logger.h"
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

extern "C" {
#include <libavutil/log.h>
}

static std::recursive_mutex log_mutex;

thread_local Side log_side = NONE;

std::unordered_map<Side, std::unordered_set<std::string>> logged_search_strings_per_side;
std::unordered_set<std::string> search_strings = {"No accelerated colorspace conversion found from"};

const char* to_string(Side side) {
  switch (side) {
    case LEFT:
      return "LEFT ";
    case RIGHT:
      return "RIGHT";
    default:
      return "UNKNN";
  }
}

void sa_av_log_callback(void* ptr, int level, const char* fmt, va_list args) {
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
    if (!logged_search_strings_per_side[log_side].insert(message).second) {
      return;
    }
  }

  if (level <= av_log_get_level() && log_side > NONE) {
    std::cerr << "[" << to_string(log_side) << "] ";
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
