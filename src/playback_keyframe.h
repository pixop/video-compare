#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
extern "C" {
#include <libavformat/version.h>
}
#include "core_types.h"
#include "ffmpeg.h"
#include "keyframe_index.h"
#include "keyframe_index_probe.h"
#include "playback_seek.h"
#include "playback_timing.h"

namespace playback_keyframe {

inline bool libavformat_index_api_available() {
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(59, 0, 0)
  return true;
#else
  return false;
#endif
}

// Index timestamps already include the stream start time. Demuxer seek positions
// use the same absolute timeline; only displayed frame PTS is start-normalized.
inline int64_t demuxer_seconds_to_stream_timestamp(const float demuxer_seconds, const AVRational& time_base) {
  const int64_t av_time = static_cast<int64_t>(static_cast<double>(demuxer_seconds) * static_cast<double>(AV_TIME_BASE));
  return av_rescale_q(av_time, AV_TIME_BASE_Q, time_base);
}

inline float stream_timestamp_to_demuxer_seconds(const int64_t stream_timestamp, const AVRational& time_base) {
  const int64_t av_time = av_rescale_q(stream_timestamp, time_base, AV_TIME_BASE_Q);
  return static_cast<float>(av_time) * AV_TIME_TO_SEC;
}

inline int find_reference_index(const KeyframeIndexSnapshot& snapshot, const int64_t reference_presentation_timestamp) {
  if (snapshot.entries.empty()) {
    return -1;
  }

  int best = -1;
  for (size_t i = 0; i < snapshot.entries.size(); ++i) {
    const IndexEntry& entry = snapshot.entries[i];
    if ((entry.flags & AVINDEX_KEYFRAME) == 0) {
      continue;
    }
    const int64_t presentation_timestamp = index_entry_presentation_timestamp(snapshot, entry);
    if (presentation_timestamp == AV_NOPTS_VALUE) {
      continue;
    }
    if (presentation_timestamp <= reference_presentation_timestamp) {
      best = static_cast<int>(i);
    } else {
      break;
    }
  }

  return best;
}

inline AdjacentKeyframe find_adjacent_keyframe(const KeyframeIndexSnapshot& snapshot, const int direction, const float reference_demuxer_seconds) {
  AdjacentKeyframe result;

  if (!snapshot_has_keyframe_index(snapshot) || direction == 0) {
    return result;
  }

  const AVRational& time_base = snapshot.time_base;
  const int64_t reference_presentation_timestamp = demuxer_seconds_to_stream_timestamp(reference_demuxer_seconds, time_base);
  const int reference_index = find_reference_index(snapshot, reference_presentation_timestamp);
  const int count = static_cast<int>(snapshot.entries.size());

  if (reference_index < 0) {
    if (direction > 0) {
      for (int i = 0; i < count; ++i) {
        const IndexEntry& entry = snapshot.entries[static_cast<size_t>(i)];
        if ((entry.flags & AVINDEX_KEYFRAME) == 0) {
          continue;
        }
        const int64_t presentation_timestamp = index_entry_presentation_timestamp(snapshot, entry);
        if (presentation_timestamp == AV_NOPTS_VALUE) {
          return result;
        }
        if (presentation_timestamp < reference_presentation_timestamp) {
          continue;
        }
        result.status = AdjacentKeyframeStatus::Ok;
        result.stream_timestamp = presentation_timestamp;
        result.demuxer_position_seconds = stream_timestamp_to_demuxer_seconds(presentation_timestamp, time_base);
        return result;
      }
      result.status = AdjacentKeyframeStatus::AtBoundary;
      return result;
    }

    result.status = AdjacentKeyframeStatus::AtBoundary;
    return result;
  }

  const IndexEntry& reference_entry = snapshot.entries[static_cast<size_t>(reference_index)];
  const int64_t reference_entry_presentation = index_entry_presentation_timestamp(snapshot, reference_entry);
  if (reference_entry_presentation == AV_NOPTS_VALUE) {
    return result;
  }
  const bool reference_is_keyframe = (reference_entry.flags & AVINDEX_KEYFRAME) != 0;
  const bool playhead_after_index_time = reference_presentation_timestamp > reference_entry_presentation;

  if (direction < 0) {
    if (reference_is_keyframe && playhead_after_index_time) {
      result.status = AdjacentKeyframeStatus::Ok;
      result.stream_timestamp = reference_entry_presentation;
      result.demuxer_position_seconds = stream_timestamp_to_demuxer_seconds(reference_entry_presentation, time_base);
      return result;
    }

    for (int i = reference_index - 1; i >= 0; --i) {
      const IndexEntry& entry = snapshot.entries[static_cast<size_t>(i)];
      if ((entry.flags & AVINDEX_KEYFRAME) == 0) {
        continue;
      }

      const int64_t presentation_timestamp = index_entry_presentation_timestamp(snapshot, entry);
      if (presentation_timestamp == AV_NOPTS_VALUE) {
        return result;
      }
      result.status = AdjacentKeyframeStatus::Ok;
      result.stream_timestamp = presentation_timestamp;
      result.demuxer_position_seconds = stream_timestamp_to_demuxer_seconds(presentation_timestamp, time_base);
      return result;
    }

    result.status = AdjacentKeyframeStatus::AtBoundary;
    return result;
  }

  const int start_index = reference_index + 1;

  for (int i = std::max(0, start_index); i < count; ++i) {
    const IndexEntry& entry = snapshot.entries[static_cast<size_t>(i)];
    if ((entry.flags & AVINDEX_KEYFRAME) == 0) {
      continue;
    }

    const int64_t presentation_timestamp = index_entry_presentation_timestamp(snapshot, entry);
    if (presentation_timestamp == AV_NOPTS_VALUE) {
      return result;
    }
    if (presentation_timestamp < reference_presentation_timestamp) {
      continue;
    }
    if (presentation_timestamp == reference_presentation_timestamp && i <= reference_index) {
      continue;
    }

    result.status = AdjacentKeyframeStatus::Ok;
    result.stream_timestamp = presentation_timestamp;
    result.demuxer_position_seconds = stream_timestamp_to_demuxer_seconds(presentation_timestamp, time_base);
    return result;
  }

  result.status = AdjacentKeyframeStatus::AtBoundary;
  return result;
}

inline bool demuxer_positions_near(const float a, const float b) {
  return std::fabs(a - b) <= 2e-3F;
}

inline float right_target_position_for_seek_relative(const playback_seek::SeekRequest& request, const size_t active_right_index, const float seek_relative) {
  playback_seek::SeekRequest probe = request;
  probe.seek_relative = seek_relative;
  probe.seek_from_start = false;
  probe.shift_right_frames = 0;

  const playback_seek::SeekPlan plan = playback_seek::plan_seek(probe);
  if (active_right_index >= plan.rights.size()) {
    return 0.0F;
  }

  return plan.rights[active_right_index].target_position;
}

struct SeekRelativeSolveResult {
  bool ok{false};
  float seek_relative{0.0F};
};

inline SeekRelativeSolveResult seek_relative_for_right_demuxer_position_bisection(const playback_seek::SeekRequest& request, const size_t active_right_index, const float desired_right_demuxer_position, const float search_span_seconds) {
  SeekRelativeSolveResult solved;

  const float current = right_target_position_for_seek_relative(request, active_right_index, 0.0F);
  const float delta = desired_right_demuxer_position - current;
  const float span = std::max(search_span_seconds, std::max(std::fabs(delta) * 2.0F, 2.0F));

  float lo = std::min(0.0F, delta) - span;
  float hi = std::max(0.0F, delta) + span;

  float lo_position = right_target_position_for_seek_relative(request, active_right_index, lo);
  float hi_position = right_target_position_for_seek_relative(request, active_right_index, hi);

  for (int expansion = 0; expansion < 8 && (lo_position > desired_right_demuxer_position || hi_position < desired_right_demuxer_position); ++expansion) {
    if (lo_position > desired_right_demuxer_position) {
      lo -= span;
      lo_position = right_target_position_for_seek_relative(request, active_right_index, lo);
    }
    if (hi_position < desired_right_demuxer_position) {
      hi += span;
      hi_position = right_target_position_for_seek_relative(request, active_right_index, hi);
    }
  }

  if (lo_position > desired_right_demuxer_position || hi_position < desired_right_demuxer_position) {
    return solved;
  }

  for (int iteration = 0; iteration < 16; ++iteration) {
    const float mid = (lo + hi) * 0.5F;
    const float position = right_target_position_for_seek_relative(request, active_right_index, mid);

    if (position < desired_right_demuxer_position) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  solved.seek_relative = (lo + hi) * 0.5F;
  const float resolved = right_target_position_for_seek_relative(request, active_right_index, solved.seek_relative);
  solved.ok = demuxer_positions_near(resolved, desired_right_demuxer_position);
  return solved;
}

inline SeekRelativeSolveResult seek_relative_for_right_demuxer_position(const playback_seek::SeekRequest& request, const size_t active_right_index, const float desired_right_demuxer_position, const float search_span_seconds = 0.0F) {
  SeekRelativeSolveResult solved;

  if (active_right_index >= request.rights.size()) {
    return solved;
  }

  const float current = right_target_position_for_seek_relative(request, active_right_index, 0.0F);
  if (demuxer_positions_near(current, desired_right_demuxer_position)) {
    solved.ok = true;
    solved.seek_relative = 0.0F;
    return solved;
  }

  if (request.time_shift_multiplier.num == request.time_shift_multiplier.den) {
    const playback_seek::SeekPlan zero_plan = playback_seek::plan_seek(request);
    const float restore = zero_plan.rights[active_right_index].restore_position;
    const float static_shift = static_cast<float>(request.unadjusted_static_right_time_shift) * AV_TIME_TO_SEC;
    solved.seek_relative = desired_right_demuxer_position - restore - static_shift;
    const float resolved = right_target_position_for_seek_relative(request, active_right_index, solved.seek_relative);
    if (demuxer_positions_near(resolved, desired_right_demuxer_position)) {
      solved.ok = true;
      return solved;
    }
  }

  return seek_relative_for_right_demuxer_position_bisection(request, active_right_index, desired_right_demuxer_position, search_span_seconds);
}

inline float current_right_demuxer_position(const playback_seek::SeekRequest& request, const size_t active_right_index) {
  return right_target_position_for_seek_relative(request, active_right_index, 0.0F);
}

inline float left_target_position_for_seek_relative(const playback_seek::SeekRequest& request, const float seek_relative) {
  playback_seek::SeekRequest probe = request;
  probe.seek_relative = seek_relative;
  probe.seek_from_start = false;
  probe.shift_right_frames = 0;
  return playback_seek::plan_seek(probe).left_target_position;
}

inline bool keyframe_navigation_left_target_inverse_supported(const playback_seek::SeekRequest& request) {
  if (request.left.single_frame || request.seek_from_start || request.shift_right_frames != 0) {
    return false;
  }
  return true;
}

inline bool keyframe_navigation_seek_plan_supported(const playback_seek::SeekRequest& request, const size_t active_right_index) {
  if (!keyframe_navigation_left_target_inverse_supported(request)) {
    return false;
  }
  if (active_right_index < request.rights.size() && request.rights[active_right_index].single_frame) {
    return false;
  }
  return true;
}

inline SeekRelativeSolveResult seek_relative_for_left_demuxer_position(const playback_seek::SeekRequest& request, const float desired_left_demuxer_position, const float search_span_seconds = 0.0F) {
  SeekRelativeSolveResult solved;
  (void)search_span_seconds;

  if (!keyframe_navigation_left_target_inverse_supported(request)) {
    return solved;
  }

  const float restore = request.left_pts * AV_TIME_TO_SEC + request.left.start_time;
  const float min_left_position = (request.left.first_pts > INT64_MIN) ? (request.left.first_pts * AV_TIME_TO_SEC + request.left.start_time) : request.left.start_time;

  const float current = left_target_position_for_seek_relative(request, 0.0F);
  if (demuxer_positions_near(current, desired_left_demuxer_position)) {
    solved.ok = true;
    solved.seek_relative = 0.0F;
    return solved;
  }

  if (desired_left_demuxer_position < min_left_position - 2e-3F) {
    return solved;
  }

  solved.seek_relative = desired_left_demuxer_position - restore;
  const float resolved = left_target_position_for_seek_relative(request, solved.seek_relative);
  solved.ok = demuxer_positions_near(resolved, desired_left_demuxer_position);
  return solved;
}

struct MappedPosition {
  bool ok{false};
  float seconds{0.0F};
};

inline MappedPosition comparison_timeline_for_right_demuxer_position(const playback_seek::SeekRequest& request, const size_t active_right_index, const float right_demuxer_position, const float search_span_seconds) {
  MappedPosition mapped;
  if (active_right_index >= request.rights.size()) {
    return mapped;
  }
  const SeekRelativeSolveResult solved = seek_relative_for_right_demuxer_position(request, active_right_index, right_demuxer_position, search_span_seconds);
  if (!solved.ok) {
    return mapped;
  }
  mapped.ok = true;
  mapped.seconds = left_target_position_for_seek_relative(request, solved.seek_relative);
  return mapped;
}

inline MappedPosition right_demuxer_for_comparison_timeline(const playback_seek::SeekRequest& request, const size_t active_right_index, const float comparison_timeline_position, const float search_span_seconds) {
  MappedPosition mapped;
  if (active_right_index >= request.rights.size()) {
    return mapped;
  }
  const SeekRelativeSolveResult solved = seek_relative_for_left_demuxer_position(request, comparison_timeline_position, search_span_seconds);
  if (!solved.ok) {
    return mapped;
  }
  mapped.ok = true;
  mapped.seconds = right_target_position_for_seek_relative(request, active_right_index, solved.seek_relative);
  return mapped;
}

struct KeyframeNavigationCursor {
  bool valid{false};
  size_t right_index{0};
  float comparison_timeline_seconds{0.0F};
};

inline void reset_keyframe_navigation_cursor(KeyframeNavigationCursor& cursor) {
  cursor.valid = false;
}

inline void commit_keyframe_navigation_cursor(KeyframeNavigationCursor& cursor, const size_t right_index, const float comparison_timeline_seconds) {
  cursor.valid = true;
  cursor.right_index = right_index;
  cursor.comparison_timeline_seconds = comparison_timeline_seconds;
}

inline bool keyframe_navigation_cursor_applies(const KeyframeNavigationCursor& cursor, const size_t active_right_index) {
  return cursor.valid && cursor.right_index == active_right_index;
}

// Callers check for AV_NOPTS_VALUE and carry an explicit validity flag.
inline float avframe_pts_to_demuxer_seconds(const int64_t frame_pts, const float video_start_time_seconds) {
  return static_cast<float>(frame_pts) * AV_TIME_TO_SEC + video_start_time_seconds;
}

inline float keyframe_navigation_reference_comparison_timeline(const KeyframeNavigationCursor& cursor,
                                                               const size_t active_right_index,
                                                               const playback_seek::SeekRequest& request,
                                                               const float displayed_left_demuxer_seconds,
                                                               const bool displayed_left_valid,
                                                               const float displayed_right_demuxer_seconds,
                                                               const bool displayed_right_valid,
                                                               const float search_span_seconds) {
  if (keyframe_navigation_cursor_applies(cursor, active_right_index)) {
    return cursor.comparison_timeline_seconds;
  }

  if (displayed_left_valid) {
    return displayed_left_demuxer_seconds;
  }

  if (displayed_right_valid) {
    const MappedPosition mapped = comparison_timeline_for_right_demuxer_position(request, active_right_index, displayed_right_demuxer_seconds, search_span_seconds);
    if (mapped.ok) {
      return mapped.seconds;
    }
  }

  return left_target_position_for_seek_relative(request, 0.0F);
}

struct UnionRightReference {
  bool query_adjacent{false};
  float demuxer_seconds{0.0F};
};

// Comparison timeline is not always a left demuxer second (e.g. sparse-left / dense-right). Use the
// displayed left playhead or seek-plan inverse for index neighbor queries.
inline float union_left_adjacent_reference_demuxer_seconds(const playback_seek::SeekRequest& request,
                                                           const float reference_comparison_timeline,
                                                           const float displayed_left_demuxer_seconds,
                                                           const bool displayed_left_valid,
                                                           const float search_span_seconds) {
  if (displayed_left_valid) {
    return displayed_left_demuxer_seconds;
  }

  const SeekRelativeSolveResult solved = seek_relative_for_left_demuxer_position(request, reference_comparison_timeline, search_span_seconds);
  if (solved.ok) {
    return left_target_position_for_seek_relative(request, solved.seek_relative);
  }

  return reference_comparison_timeline;
}

// After landing on a right-owned union vertex, a forward adjacent query at the mapped right
// demuxer can repeat the same key; nudge just beyond the 2 ms equality tolerance.
inline float union_right_adjacent_reference_demuxer_seconds(const playback_seek::SeekRequest& request,
                                                            const size_t active_right_index,
                                                            const float reference_comparison_timeline,
                                                            const int step_direction,
                                                            const float mapped_right_demuxer_seconds,
                                                            const float search_span_seconds) {
  if (step_direction == 0) {
    return mapped_right_demuxer_seconds;
  }

  constexpr float kAdjacentComparisonBias = 2.1e-3F;
  const float shifted_comparison = reference_comparison_timeline + static_cast<float>(step_direction) * kAdjacentComparisonBias;
  const MappedPosition shifted = right_demuxer_for_comparison_timeline(request, active_right_index, shifted_comparison, search_span_seconds);
  if (!shifted.ok) {
    return mapped_right_demuxer_seconds;
  }

  if (step_direction > 0) {
    return std::max(mapped_right_demuxer_seconds, shifted.seconds);
  }

  return std::min(mapped_right_demuxer_seconds, shifted.seconds);
}

inline UnionRightReference right_reference_for_union_navigation(const KeyframeNavigationCursor& cursor,
                                                                const size_t active_right_index,
                                                                const playback_seek::SeekRequest& request,
                                                                const float reference_comparison_timeline,
                                                                const bool right_indexed,
                                                                const bool displayed_right_valid,
                                                                const float displayed_right_demuxer_seconds,
                                                                const float search_span_seconds) {
  UnionRightReference reference;
  if (!right_indexed) {
    return reference;
  }

  const MappedPosition mapped = right_demuxer_for_comparison_timeline(request, active_right_index, reference_comparison_timeline, search_span_seconds);
  if (mapped.ok) {
    reference.query_adjacent = true;
    reference.demuxer_seconds = mapped.seconds;
    return reference;
  }

  if (!keyframe_navigation_cursor_applies(cursor, active_right_index) && displayed_right_valid) {
    reference.query_adjacent = true;
    reference.demuxer_seconds = displayed_right_demuxer_seconds;
    return reference;
  }

  return reference;
}

struct UnionKeyframeCandidate {
  SideType owner{SideType::Left};
  float owner_demuxer_position{0.0F};
  float comparison_timeline_position{0.0F};
};

struct UnionKeyframeSelection {
  bool found{false};
  bool at_boundary{false};
  UnionKeyframeCandidate candidate;
};

inline bool union_candidates_share_timeline(const UnionKeyframeCandidate& a, const UnionKeyframeCandidate& b) {
  return demuxer_positions_near(a.comparison_timeline_position, b.comparison_timeline_position);
}

inline UnionKeyframeSelection select_union_keyframe_candidate(const int direction,
                                                              const float reference_comparison_timeline,
                                                              const AdjacentKeyframe& left_adjacent,
                                                              const AdjacentKeyframe& right_adjacent,
                                                              const bool left_indexed,
                                                              const bool right_indexed,
                                                              const playback_seek::SeekRequest& request,
                                                              const size_t active_right_index,
                                                              const float search_span_seconds) {
  UnionKeyframeSelection result;

  if (direction == 0) {
    return result;
  }

  std::vector<UnionKeyframeCandidate> candidates;
  // Set when a right Ok adjacent fails comparison-timeline mapping for reasons other than
  // lying before the reachable lower bound (see right_before_reachable_timeline_start).
  bool right_mapping_failed = false;
  bool right_before_reachable_timeline_start = false;
  bool left_before_right_reachable_start = false;

  auto try_add_candidate = [&](const SideType owner, const AdjacentKeyframe& adjacent) {
    if (adjacent.status != AdjacentKeyframeStatus::Ok) {
      return;
    }
    UnionKeyframeCandidate candidate;
    candidate.owner = owner;
    candidate.owner_demuxer_position = adjacent.demuxer_position_seconds;
    if (owner == SideType::Left) {
      candidate.comparison_timeline_position = adjacent.demuxer_position_seconds;
      if (direction < 0 && active_right_index < request.rights.size()) {
        const playback_seek::SeekSideInput& right = request.rights[active_right_index];
        const float min_right_position = (right.first_pts > INT64_MIN) ? (right.first_pts * AV_TIME_TO_SEC + right.start_time) : right.start_time;
        const MappedPosition mapped_right = right_demuxer_for_comparison_timeline(request, active_right_index, adjacent.demuxer_position_seconds, search_span_seconds);
        if (mapped_right.ok && mapped_right.seconds < min_right_position - 2e-3F) {
          left_before_right_reachable_start = true;
          return;
        }
      }
    } else {
      const MappedPosition mapped = comparison_timeline_for_right_demuxer_position(request, active_right_index, adjacent.demuxer_position_seconds, search_span_seconds);
      if (!mapped.ok) {
        bool before_reachable_start = false;
        if (direction < 0 && active_right_index < request.rights.size() && left_adjacent.status == AdjacentKeyframeStatus::AtBoundary) {
          const float min_left_position = (request.left.first_pts > INT64_MIN) ? (request.left.first_pts * AV_TIME_TO_SEC + request.left.start_time) : request.left.start_time;
          const SeekRelativeSolveResult solved = seek_relative_for_left_demuxer_position(request, min_left_position, search_span_seconds);
          if (solved.ok) {
            const float earliest_right = right_target_position_for_seek_relative(request, active_right_index, solved.seek_relative);
            if (adjacent.demuxer_position_seconds < earliest_right - 2e-3F) {
              before_reachable_start = true;
            }
          }
        }
        if (before_reachable_start) {
          right_before_reachable_timeline_start = true;
        } else {
          right_mapping_failed = true;
        }
        return;
      }
      candidate.comparison_timeline_position = mapped.seconds;
    }
    for (UnionKeyframeCandidate& existing : candidates) {
      if (union_candidates_share_timeline(existing, candidate)) {
        if (candidate.owner == SideType::Left) {
          existing = candidate;
        }
        return;
      }
    }
    candidates.push_back(candidate);
  };

  if (left_indexed) {
    try_add_candidate(SideType::Left, left_adjacent);
  }
  if (right_indexed) {
    try_add_candidate(SideType::Right, right_adjacent);
  }

  if (candidates.empty()) {
    if (right_mapping_failed) {
      return result;
    }

    const bool left_unavail = left_indexed && left_adjacent.status == AdjacentKeyframeStatus::Unavailable;
    const bool right_unavail = right_indexed && right_adjacent.status == AdjacentKeyframeStatus::Unavailable;
    if (left_unavail || right_unavail) {
      return result;
    }

    const bool left_at_union_boundary = !left_indexed || left_adjacent.status == AdjacentKeyframeStatus::AtBoundary || (direction < 0 && left_before_right_reachable_start);
    const bool right_at_union_boundary = !right_indexed || right_adjacent.status == AdjacentKeyframeStatus::AtBoundary || (direction < 0 && right_before_reachable_timeline_start);
    const bool all_indexed_at_boundary = left_at_union_boundary && right_at_union_boundary;
    if ((left_indexed || right_indexed) && all_indexed_at_boundary) {
      result.at_boundary = true;
    }
    return result;
  }

  UnionKeyframeCandidate* best = nullptr;
  float best_distance = 0.0F;

  for (UnionKeyframeCandidate& candidate : candidates) {
    if (demuxer_positions_near(candidate.comparison_timeline_position, reference_comparison_timeline)) {
      continue;
    }

    const float delta = candidate.comparison_timeline_position - reference_comparison_timeline;
    if (direction > 0 && delta <= 0.0F) {
      continue;
    }
    if (direction < 0 && delta >= 0.0F) {
      continue;
    }

    const float distance = std::fabs(delta);
    if (best == nullptr || distance < best_distance - 1e-6F || (std::fabs(distance - best_distance) <= 1e-6F && candidate.owner == SideType::Left && best->owner != SideType::Left)) {
      best = &candidate;
      best_distance = distance;
    }
  }

  if (best == nullptr) {
    result.at_boundary = true;
    return result;
  }

  result.found = true;
  result.candidate = *best;
  return result;
}

}  // namespace playback_keyframe
