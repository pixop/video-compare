#include "frame_metadata.h"
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string>
#include "ffmpeg.h"
extern "C" {
#include <libavutil/dict.h>
#include <libavutil/frame.h>
}

namespace FrameMetadata {
namespace {

constexpr const char* KEY = "frame_key";
constexpr const char* FILTER_GENERATION = "filter_generation";
constexpr const char* RESOLVED_FILTERS = "resolved_filters";
constexpr const char* ORIGINAL_WIDTH = "original_width";
constexpr const char* ORIGINAL_HEIGHT = "original_height";

const char* find_cstr(const AVFrame* frame, const char* key) {
  if (frame == nullptr) {
    return nullptr;
  }

  const AVDictionaryEntry* entry = av_dict_get(frame->metadata, key, nullptr, 0);
  return entry != nullptr ? entry->value : nullptr;
}

int get_int(const AVFrame* frame, const char* key, int default_value) {
  const char* value = find_cstr(frame, key);
  if (value == nullptr || value[0] == '\0') {
    return default_value;
  }

  errno = 0;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || *end != '\0' || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX) {
    return default_value;
  }

  return static_cast<int>(parsed);
}

void set_string(AVFrame* frame, const char* key, const std::string& value) {
  assert(frame != nullptr);
  ffmpeg::check(av_dict_set(&frame->metadata, key, value.c_str(), 0));
}

void set_int(AVFrame* frame, const char* key, const int64_t value) {
  assert(frame != nullptr);
  ffmpeg::check(av_dict_set_int(&frame->metadata, key, value, 0));
}

}  // namespace

std::string require_key(const AVFrame* frame) {
  assert(frame != nullptr);

  const char* value = find_cstr(frame, KEY);
  if (value == nullptr || value[0] == '\0') {
    throw ffmpeg::Error{"required frame_key metadata is missing"};
  }

  return value;
}

void set_key(AVFrame* frame, const std::string& key) {
  set_string(frame, KEY, key);
}

int get_filter_generation(const AVFrame* frame) {
  return get_int(frame, FILTER_GENERATION, -1);
}

void set_filter_generation(AVFrame* frame, int generation) {
  set_int(frame, FILTER_GENERATION, generation);
}

std::string get_resolved_filters(const AVFrame* frame) {
  const char* value = find_cstr(frame, RESOLVED_FILTERS);
  return value != nullptr ? value : "";
}

void set_resolved_filters(AVFrame* frame, const std::string& filters) {
  set_string(frame, RESOLVED_FILTERS, filters);
}

int get_original_width(const AVFrame* frame, int fallback) {
  return get_int(frame, ORIGINAL_WIDTH, fallback);
}

int get_original_height(const AVFrame* frame, int fallback) {
  return get_int(frame, ORIGINAL_HEIGHT, fallback);
}

void set_original_dimensions(AVFrame* frame, int width, int height) {
  assert(frame != nullptr);
  set_int(frame, ORIGINAL_WIDTH, width);
  set_int(frame, ORIGINAL_HEIGHT, height);
}

std::string make_frame_key(const int64_t pts, int filter_generation) {
  const int generation = filter_generation < 0 ? 0 : filter_generation;
  return std::to_string(pts) + ":" + std::to_string(generation);
}

std::string make_frame_key(const AVFrame* frame) {
  assert(frame != nullptr);
  return make_frame_key(frame->pts, get_filter_generation(frame));
}

}  // namespace FrameMetadata
