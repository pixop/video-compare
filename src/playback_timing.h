#pragma once

#include <algorithm>
#include <cstdint>
#include "ffmpeg.h"

namespace playback_timing {

inline bool is_behind(int64_t frame1_pts, int64_t frame2_pts, int64_t delta_pts) {
  const float t1 = static_cast<float>(frame1_pts) * AV_TIME_TO_SEC;
  const float t2 = static_cast<float>(frame2_pts) * AV_TIME_TO_SEC;
  const float delta_s = static_cast<float>(delta_pts) * AV_TIME_TO_SEC - 1e-5F;

  const float diff = t1 - t2;
  const float tolerance = std::max(delta_s, 1.0F / 480.0F);

  return diff < -tolerance;
}

inline int64_t compute_min_delta(const int64_t delta_left_pts, const int64_t delta_right_pts) {
  return std::min(delta_left_pts, delta_right_pts) * 8 / 10;
}

inline bool is_in_sync(const int64_t left_pts, const int64_t right_pts, const int64_t delta_left_pts, const int64_t delta_right_pts) {
  const int64_t min_delta = compute_min_delta(delta_left_pts, delta_right_pts);

  return !is_behind(left_pts, right_pts, min_delta) && !is_behind(right_pts, left_pts, min_delta);
}

inline int64_t compute_frame_delay(const int64_t left_pts, const int64_t right_pts) {
  return std::max(left_pts, right_pts);
}

inline int64_t time_ms_to_av_time(const double time_ms) {
  return time_ms * MILLISEC_TO_AV_TIME;
}

inline int64_t calculate_dynamic_time_shift(const AVRational& multiplier, const int64_t original_pts, const bool inverse) {
  // Calculate the time shift as the difference between original and scaled PTS
  const int64_t time_shift =
      inverse ? (original_pts - av_rescale_q(original_pts, AVRational{multiplier.den, multiplier.num}, AVRational{1, 1})) : (av_rescale_q(original_pts, AVRational{multiplier.num, multiplier.den}, AVRational{1, 1}) - original_pts);

  return time_shift;
}

inline int64_t normalized_delta(const int64_t delta) {
  return delta > 0 ? delta : 10000;
}

}  // namespace playback_timing
