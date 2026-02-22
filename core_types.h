#pragma once

#include <cstddef>
#include <cstdint>
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

inline std::string get_frame_key(const AVFrame* frame) {
  const AVDictionaryEntry* frame_key_entry = av_dict_get(frame->metadata, "frame_key", nullptr, 0);
  return frame_key_entry->value;
}

inline void set_frame_key(AVFrame* frame, const std::string& frame_key) {
  av_dict_set(&frame->metadata, "frame_key", frame_key.c_str(), 0);
}
