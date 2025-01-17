#pragma once

#include <string>
#include "core_types.h"

extern thread_local Side log_side;

void sa_av_log_callback(void* ptr, int level, const char* fmt, va_list args);

void sa_log(const Side side, const std::string& message);
void sa_log_info(const Side side, const std::string& message);
void sa_log_warning(const Side side, const std::string& message);
void sa_log_error(const Side side, const std::string& message);

class ScopedLogSide {
  const Side previous_side_;

 public:
  ScopedLogSide(const Side new_side) : previous_side_(log_side) { log_side = new_side; }
  ~ScopedLogSide() { log_side = previous_side_; }
};
