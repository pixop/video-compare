#pragma once

#include <cassert>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
extern "C" {
#include <libavutil/dict.h>
#include <libavutil/frame.h>
}

constexpr int64_t MAX_AVRATIONAL_REDUCE = 1024 * 1024;

constexpr unsigned UNSET_PEAK_LUMINANCE = 0;

enum class ToneMapping { Auto, Off, FullRange, Relative };
enum class DynamicRange { Standard, PQ, HLG };
enum class FontMode { Auto, SourceCodePro, Sarasa, CustomFile };

// Auto, SourceCodePro, and Sarasa use an empty custom_file_path.
// CustomFile requires a non-empty custom_file_path.
struct FontSelection {
  FontMode mode{FontMode::Auto};
  std::string custom_file_path;
};

enum class SideType { None = -1, Left, Right };

class Side {
 public:
  // Constructors
  Side();
  explicit Side(SideType type, size_t right_index = 0);

  // Static factory methods for convenience
  static Side Left();
  static Side Right(size_t index = 0);
  static Side None();

  // Accessors (kept inline for performance)
  SideType type() const { return type_; }
  size_t right_index() const { return right_index_; }
  bool is_left() const { return type_ == SideType::Left; }
  bool is_right() const { return type_ == SideType::Right; }
  bool is_none() const { return type_ == SideType::None; }
  bool is_valid() const { return type_ != SideType::None; }

  // Convert to array index (for backward compatibility)
  // LEFT -> 0, RIGHT -> 1 (for first right video), or use right_index for multiple
  size_t as_index() const;

  // For use in arrays that only have LEFT/RIGHT (backward compatibility)
  size_t as_simple_index() const;

  // Comparison operators
  bool operator==(const Side& other) const;
  bool operator!=(const Side& other) const;
  bool operator<(const Side& other) const;

  // Hash support
  size_t hash() const;

  // String representation
  std::string to_string() const;

 private:
  SideType type_;
  size_t right_index_;  // Only meaningful for RIGHT, 0-based
};

// Hash function for use in unordered_map
namespace std {
template <>
struct hash<Side> {
  size_t operator()(const Side& side) const { return side.hash(); }
};
}  // namespace std

extern const Side NONE;
extern const Side LEFT;
extern const Side RIGHT;

constexpr size_t SideCount = 2;  // For arrays that only have LEFT/RIGHT

namespace FrameMetadata {

constexpr const char* KEY = "frame_key";
constexpr const char* FILTER_GENERATION = "filter_generation";
constexpr const char* RESOLVED_FILTERS = "resolved_filters";
constexpr const char* ORIGINAL_WIDTH = "original_width";
constexpr const char* ORIGINAL_HEIGHT = "original_height";

namespace detail {

inline const char* find_cstr(const AVFrame* frame, const char* key) {
  if (frame == nullptr) {
    return nullptr;
  }

  const AVDictionaryEntry* entry = av_dict_get(frame->metadata, key, nullptr, 0);
  return entry != nullptr ? entry->value : nullptr;
}

}  // namespace detail

void set_string(AVFrame* frame, const char* key, const std::string& value);
void set_int(AVFrame* frame, const char* key, int64_t value);
std::string require_key(const AVFrame* frame);

inline int get_int(const AVFrame* frame, const char* key, int default_value) {
  const char* value = detail::find_cstr(frame, key);
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

inline void set_key(AVFrame* frame, const std::string& key) {
  set_string(frame, KEY, key);
}

inline int get_filter_generation(const AVFrame* frame) {
  return get_int(frame, FILTER_GENERATION, -1);
}

inline void set_filter_generation(AVFrame* frame, int generation) {
  set_int(frame, FILTER_GENERATION, generation);
}

inline std::string get_resolved_filters(const AVFrame* frame) {
  const char* value = detail::find_cstr(frame, RESOLVED_FILTERS);
  return value != nullptr ? value : "";
}

inline void set_resolved_filters(AVFrame* frame, const std::string& filters) {
  set_string(frame, RESOLVED_FILTERS, filters);
}

inline int get_original_width(const AVFrame* frame, int fallback) {
  return get_int(frame, ORIGINAL_WIDTH, fallback);
}

inline int get_original_height(const AVFrame* frame, int fallback) {
  return get_int(frame, ORIGINAL_HEIGHT, fallback);
}

inline void set_original_dimensions(AVFrame* frame, int width, int height) {
  assert(frame != nullptr);
  set_int(frame, ORIGINAL_WIDTH, width);
  set_int(frame, ORIGINAL_HEIGHT, height);
}

inline std::string make_frame_key(int64_t pts, int filter_generation) {
  const int generation = filter_generation < 0 ? 0 : filter_generation;
  return std::to_string(pts) + ":" + std::to_string(generation);
}

inline std::string make_frame_key(const AVFrame* frame) {
  assert(frame != nullptr);
  return make_frame_key(frame->pts, get_filter_generation(frame));
}

}  // namespace FrameMetadata
