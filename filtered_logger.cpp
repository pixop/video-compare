#include "filtered_logger.h"
#include <algorithm>
#include <iostream>
extern "C" {
#include <libavutil/log.h>
}

FilteredLogger& FilteredLogger::instance() {
  static FilteredLogger logger_instance;

  return logger_instance;
}

FilteredLogger::FilteredLogger() {
  reset();
}

void FilteredLogger::install(const std::string& pattern) {
  set_regex_pattern(pattern);

  av_log_set_callback(ffmpeg_log_callback);
}

void FilteredLogger::set_regex_pattern(const std::string& pattern) {
  std::lock_guard<std::mutex> lock(mtx_);

  try {
    regex_ = std::regex(pattern);
  } catch (const std::regex_error& e) {
    std::cerr << "Error compiling regex: " << e.what() << std::endl;
    throw;
  }
}

bool FilteredLogger::log_callback(int level, const char* fmt, va_list vl) {
  if (level == AV_LOG_INFO) {
    std::string log_msg = format_message(fmt, vl);

    std::lock_guard<std::mutex> lock(mtx_);

    if (std::regex_search(log_msg, regex_)) {
      log_to_buffer(log_msg);

      return true;
    }
  }

  return false;
}

std::string FilteredLogger::format_message(const char* fmt, va_list vl) {
  va_list vl_copy;
  va_copy(vl_copy, vl);
  int needed_size = vsnprintf(nullptr, 0, fmt, vl_copy) + 1;
  va_end(vl_copy);

  if (needed_size <= 0) {
    return "";
  }

  std::vector<char> temp_buffer(needed_size);

  const size_t result = vsnprintf(temp_buffer.data(), temp_buffer.size(), fmt, vl);

  if (result < 0 || result >= temp_buffer.size()) {
    return "";
  }

  return std::string(temp_buffer.data());
}

void FilteredLogger::log_to_buffer(const std::string& msg) {
  std::size_t remaining_space = log_buffer_.size() - log_buffer_index_;
  std::size_t to_copy = std::min(msg.size(), remaining_space);

  if (to_copy > 0) {
    std::copy(msg.begin(), msg.begin() + to_copy, log_buffer_.begin() + log_buffer_index_);

    log_buffer_index_ += to_copy;
  }
}

std::string FilteredLogger::get_buffered_logs() {
  std::lock_guard<std::mutex> lock(mtx_);

  return std::string(log_buffer_.data(), log_buffer_index_);
}

void FilteredLogger::reset() {
  std::lock_guard<std::mutex> lock(mtx_);

  log_buffer_index_ = 0;
  log_buffer_.front() = '\0';
}

void FilteredLogger::ffmpeg_log_callback(void* ptr, int level, const char* fmt, va_list vl) {
  if (!instance().log_callback(level, fmt, vl)) {
    av_log_default_callback(ptr, level, fmt, vl);
  }
}