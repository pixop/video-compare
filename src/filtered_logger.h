#pragma once
#include <array>
#include <mutex>
#include <regex>
#include <string>

class FilteredLogger {
 private:
  std::array<char, 4096> log_buffer_;
  std::size_t log_buffer_index_;
  std::regex regex_;
  std::mutex mtx_;

 public:
  FilteredLogger(const FilteredLogger&) = delete;
  FilteredLogger& operator=(const FilteredLogger&) = delete;

  static FilteredLogger& instance();

  void install(const std::string& pattern);

  std::string get_buffered_logs();

  void reset();

 private:
  FilteredLogger();

  void set_regex_pattern(const std::string& pattern);

  bool log_callback(int level, const char* fmt, va_list vl);
  std::string format_message(const char* fmt, va_list vl);
  void log_to_buffer(const std::string& msg);

  // static callback function to be registered with FFmpeg
  static void ffmpeg_log_callback(void* ptr, int level, const char* fmt, va_list vl);
};