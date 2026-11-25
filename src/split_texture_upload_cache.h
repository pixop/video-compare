#pragma once

#include <algorithm>
#include <string>

// The split view shares one SDL texture between both videos. For each frame
// pair, the left side owns a valid prefix and the right side a valid suffix.
// An upload overwrites the other side's overlapping pixels.
//
// This cache only records validity; Display still performs the same
// SDL_UpdateTexture calls and retains responsibility for texture lifetime.
class SplitTextureUploadCache {
 public:
  void reset(int width) {
    width_ = width;
    left_valid_end_ = 0;
    right_valid_start_ = width;
    left_frame_ = nullptr;
    right_frame_ = nullptr;
    left_key_.clear();
    right_key_.clear();
  }

  // Frame keys do not identify the input: distinct videos may share the same
  // timestamp and filter generation. Compare both key and frame address so
  // swaps and active-right changes cannot reuse the other video's pixels.
  void set_frames(const void* left_frame, const std::string& left_key, const void* right_frame, const std::string& right_key) {
    if (left_frame != left_frame_ || left_key != left_key_) {
      left_frame_ = left_frame;
      left_key_ = left_key;
      left_valid_end_ = 0;
    }
    if (right_frame != right_frame_ || right_key != right_key_) {
      right_frame_ = right_frame;
      right_key_ = right_key;
      right_valid_start_ = width_;
    }
  }

  bool needs_left_upload(int split_x) const { return split_x > left_valid_end_; }
  bool needs_right_upload(int split_x) const { return split_x < right_valid_start_; }

  void left_uploaded(int split_x) {
    left_valid_end_ = split_x;
    right_valid_start_ = std::max(right_valid_start_, split_x);
  }

  void right_uploaded(int split_x) {
    right_valid_start_ = split_x;
    left_valid_end_ = std::min(left_valid_end_, split_x);
  }

 private:
  int width_{0};
  int left_valid_end_{0};
  int right_valid_start_{0};
  const void* left_frame_{nullptr};
  const void* right_frame_{nullptr};
  std::string left_key_, right_key_;
};
