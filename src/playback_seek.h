#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>
#include "ffmpeg.h"
#include "playback_timing.h"

namespace playback_seek {

struct SeekSideInput {
  float start_time;
  int64_t first_pts;  // INT64_MIN when unknown
  int64_t delta_pts;
  bool single_frame;
};

struct SeekRequest {
  float seek_relative;
  bool seek_from_start;

  bool force_seek_current_position;
  bool all_sides_multi_frame;
  int shift_right_frames;

  double shortest_duration;

  int64_t left_pts;

  int64_t unadjusted_static_right_time_shift;
  AVRational time_shift_multiplier;

  SeekSideInput left;
  std::vector<SeekSideInput> rights;
};

struct RightSeekTarget {
  float target_position;
  float restore_position;
};

struct SeekPlan {
  float left_target_position;
  float left_restore_position;
  std::vector<RightSeekTarget> rights;
  bool backward;
};

inline SeekPlan plan_seek(const SeekRequest& request) {
  float seek_relative = request.seek_relative;

  SeekPlan plan;
  plan.left_restore_position = request.left_pts * AV_TIME_TO_SEC + request.left.start_time;

  const float min_left_position = (request.left.first_pts > INT64_MIN) ? (request.left.first_pts * AV_TIME_TO_SEC + request.left.start_time) : request.left.start_time;

  if (request.seek_from_start && !request.left.single_frame) {
    plan.left_target_position = request.shortest_duration * seek_relative + request.left.start_time;
  } else {
    if (request.left.single_frame) {
      seek_relative = request.left.delta_pts * AV_TIME_TO_SEC;
    }

    plan.left_target_position = plan.left_restore_position + seek_relative;
  }

  plan.left_target_position = std::max(plan.left_target_position, min_left_position);

  plan.backward = (seek_relative < 0.0F) || (request.shift_right_frames != 0) || (request.force_seek_current_position && request.all_sides_multi_frame);

  plan.rights.reserve(request.rights.size());

  for (const SeekSideInput& right : request.rights) {
    RightSeekTarget target;
    target.restore_position = request.left_pts * AV_TIME_TO_SEC + right.start_time;

    const float min_right_position = (right.first_pts > INT64_MIN) ? (right.first_pts * AV_TIME_TO_SEC + right.start_time) : right.start_time;

    if (request.seek_from_start && !right.single_frame) {
      target.target_position = request.shortest_duration * seek_relative + right.start_time;
    } else {
      if (right.single_frame) {
        seek_relative = right.delta_pts * AV_TIME_TO_SEC;
      }

      target.target_position = target.restore_position + seek_relative;
    }

    target.target_position = std::max(target.target_position, min_right_position);

    target.target_position += request.unadjusted_static_right_time_shift * AV_TIME_TO_SEC;
    target.target_position += static_cast<float>(playback_timing::calculate_dynamic_time_shift(request.time_shift_multiplier, (target.target_position - right.start_time) / AV_TIME_TO_SEC, false)) * AV_TIME_TO_SEC;

    plan.rights.push_back(target);
  }

  return plan;
}

inline int64_t compute_post_seek_static_right_time_shift(int64_t shift) {
  if (shift > 0) {
    shift = ((shift / 1000) + 2) * 1000;
  } else if (shift < 0) {
    shift = ((shift / 1000) - 2) * 1000;
  }

  return shift;
}

}  // namespace playback_seek
