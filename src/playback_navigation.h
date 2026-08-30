#pragma once

#include <algorithm>
#include <cstdint>
#include "ffmpeg.h"

namespace playback_navigation {

inline float seek_relative_after_backward_navigation(const float seek_relative, const int frame_navigation_delta, const int64_t frame_delta) {
  return seek_relative + static_cast<float>(frame_navigation_delta) * (static_cast<float>(frame_delta) * AV_TIME_TO_SEC);
}

inline int adjusted_frame_offset(const int current_offset, const int adjustment, const int last_common_index) {
  return std::min(std::max(0, current_offset + adjustment), last_common_index);
}

inline int next_forward_only_offset(const int current_offset, const int last_common_index) {
  if (current_offset == 0) {
    return last_common_index;
  }

  return adjusted_frame_offset(current_offset, -1, last_common_index);
}

struct PingPongStep {
  int offset;
  bool forward;
};

inline PingPongStep next_ping_pong_step(const int current_offset, const int last_common_index, bool forward) {
  if (last_common_index >= 1 && (current_offset == 0 || current_offset == last_common_index)) {
    forward = !forward;
  }

  return PingPongStep{adjusted_frame_offset(current_offset, forward ? -1 : 1, last_common_index), forward};
}

}  // namespace playback_navigation
