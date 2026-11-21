#include "keyframe_navigation.h"

#include <cmath>
#include "demuxer.h"

namespace playback_keyframe {

DisplayedDemuxerAnchors displayed_demuxer_anchors(const AVFrame* left_frame, const Demuxer& left_demuxer, const AVFrame* right_frame, const Demuxer& right_demuxer) {
  DisplayedDemuxerAnchors anchors;

  if (left_frame != nullptr && left_frame->pts != AV_NOPTS_VALUE) {
    anchors.left_demuxer_seconds = avframe_pts_to_demuxer_seconds(left_frame->pts, left_demuxer.video_start_time_seconds());
    anchors.left_valid = true;
  }

  if (right_frame != nullptr && right_frame->pts != AV_NOPTS_VALUE) {
    anchors.right_demuxer_seconds = avframe_pts_to_demuxer_seconds(right_frame->pts, right_demuxer.video_start_time_seconds());
    anchors.right_valid = true;
  }

  return anchors;
}

const char* keyframe_navigation_message(const KeyframeNavigationStatus status) {
  switch (status) {
    case KeyframeNavigationStatus::Unavailable:
    case KeyframeNavigationStatus::SeekSolveFailed:
    case KeyframeNavigationStatus::LandingFailed:
      return "Keyframe navigation unavailable";
    case KeyframeNavigationStatus::AtBoundary:
      return "No more keyframes in this direction";
    default:
      return nullptr;
  }
}

void KeyframeNavigation::reset_session() {
  reset_keyframe_navigation_cursor(cursor_);
  sync_pending_ = false;
  pending_steps_.clear();
}

void KeyframeNavigation::enqueue_steps(const int delta) {
  if (delta == 0) {
    return;
  }
  const int direction = delta > 0 ? 1 : -1;
  const int steps = std::abs(delta);
  for (int i = 0; i < steps && pending_steps_.size() < kMaxPendingSteps; ++i) {
    pending_steps_.push_back(direction);
  }
}

KeyframeNavigationBegin KeyframeNavigation::begin_frame(const int keyframe_navigation_delta, const bool in_sync_when_pending) {
  KeyframeNavigationBegin result;

  if (keyframe_navigation_delta != 0) {
    const size_t pending_before = pending_steps_.size();
    enqueue_steps(keyframe_navigation_delta);
    const size_t enqueued = pending_steps_.size() - pending_before;
    const int requested = std::abs(keyframe_navigation_delta);
    if (enqueued < static_cast<size_t>(requested)) {
      result.queue_overflow = requested - static_cast<int>(enqueued);
    }
  }

  if (sync_pending_ && !in_sync_when_pending) {
    return result;
  }

  if (sync_pending_ && in_sync_when_pending) {
    sync_pending_ = false;
  }

  if (!pending_steps_.empty()) {
    result.direction = pending_steps_.front();
    pending_steps_.pop_front();
  }

  return result;
}

KeyframeNavigationResolve KeyframeNavigation::resolve_step(const int step_direction,
                                                           Demuxer& left_demuxer,
                                                           Demuxer& right_demuxer,
                                                           const playback_seek::SeekRequest& seek_request,
                                                           const size_t active_right_index,
                                                           const float displayed_left_demuxer_seconds,
                                                           const bool displayed_left_valid,
                                                           const float displayed_right_demuxer_seconds,
                                                           const bool displayed_right_valid) {
  KeyframeNavigationResolve resolved;

  if (step_direction == 0) {
    return resolved;
  }

  if (!left_demuxer.keyframe_navigation_seekable() || !right_demuxer.keyframe_navigation_seekable()) {
    pending_steps_.clear();
    resolved.status = KeyframeNavigationStatus::Unavailable;
    return resolved;
  }

  const float reference_comparison_timeline = keyframe_navigation_reference_comparison_timeline(cursor_, active_right_index, seek_request, displayed_left_demuxer_seconds, displayed_left_valid, displayed_right_demuxer_seconds,
                                                                                                displayed_right_valid, seek_request.shortest_duration);

  const bool left_indexed = left_demuxer.index_navigation_supported();
  const bool right_indexed = right_demuxer.index_navigation_supported();

  if (!left_indexed && !right_indexed) {
    pending_steps_.clear();
    resolved.status = KeyframeNavigationStatus::Unavailable;
    return resolved;
  }

  if (!keyframe_navigation_seek_plan_supported(seek_request, active_right_index)) {
    pending_steps_.clear();
    resolved.status = KeyframeNavigationStatus::Unavailable;
    return resolved;
  }

  const float left_adjacent_reference = union_left_adjacent_reference_demuxer_seconds(seek_request, reference_comparison_timeline, displayed_left_demuxer_seconds, displayed_left_valid, seek_request.shortest_duration);

  const UnionRightReference right_reference =
      right_reference_for_union_navigation(cursor_, active_right_index, seek_request, reference_comparison_timeline, right_indexed, displayed_right_valid, displayed_right_demuxer_seconds, seek_request.shortest_duration);

  const AdjacentKeyframe left_adjacent = left_indexed ? left_demuxer.adjacent_keyframe(step_direction, left_adjacent_reference) : AdjacentKeyframe{};
  AdjacentKeyframe right_adjacent;
  if (right_indexed && right_reference.query_adjacent) {
    const float right_adjacent_reference = union_right_adjacent_reference_demuxer_seconds(seek_request, active_right_index, reference_comparison_timeline, step_direction, right_reference.demuxer_seconds, seek_request.shortest_duration);
    right_adjacent = right_demuxer.adjacent_keyframe(step_direction, right_adjacent_reference);
  } else if (right_indexed) {
    right_adjacent.status = AdjacentKeyframeStatus::Unavailable;
  }

  const UnionKeyframeSelection selection =
      select_union_keyframe_candidate(step_direction, reference_comparison_timeline, left_adjacent, right_adjacent, left_indexed, right_indexed, seek_request, active_right_index, seek_request.shortest_duration);

  if (!selection.found) {
    pending_steps_.clear();
    resolved.status = selection.at_boundary ? KeyframeNavigationStatus::AtBoundary : KeyframeNavigationStatus::Unavailable;
    return resolved;
  }

  const SeekRelativeSolveResult solved = seek_relative_for_left_demuxer_position(seek_request, selection.candidate.comparison_timeline_position, seek_request.shortest_duration);
  if (!solved.ok) {
    pending_steps_.clear();
    resolved.status = KeyframeNavigationStatus::SeekSolveFailed;
    return resolved;
  }

  resolved.status = KeyframeNavigationStatus::SeekReady;
  resolved.seek_relative = solved.seek_relative;
  resolved.target_demuxer_position = selection.candidate.owner_demuxer_position;
  resolved.comparison_timeline_position = selection.candidate.comparison_timeline_position;
  resolved.candidate_owner = selection.candidate.owner;
  return resolved;
}

KeyframeNavigationStatus KeyframeNavigation::verify_landing_from_frames(const KeyframeNavigationResolve& resolve,
                                                                        const size_t active_right_index,
                                                                        const AVFrame* left_frame,
                                                                        const Demuxer& left_demuxer,
                                                                        const AVFrame* right_frame,
                                                                        const Demuxer& right_demuxer) {
  const bool owner_left = resolve.candidate_owner == SideType::Left;
  float landed_demuxer = 0.0F;
  bool landed_valid = false;

  if (owner_left) {
    if (left_frame != nullptr && left_frame->pts != AV_NOPTS_VALUE) {
      landed_demuxer = avframe_pts_to_demuxer_seconds(left_frame->pts, left_demuxer.video_start_time_seconds());
      landed_valid = true;
    }
  } else if (right_frame != nullptr && right_frame->pts != AV_NOPTS_VALUE) {
    landed_demuxer = avframe_pts_to_demuxer_seconds(right_frame->pts, right_demuxer.video_start_time_seconds());
    landed_valid = true;
  }

  return verify_landing(active_right_index, landed_demuxer, landed_valid, resolve.target_demuxer_position, resolve.comparison_timeline_position);
}

KeyframeNavigationStatus KeyframeNavigation::verify_landing(const size_t active_right_index, const float landed_demuxer_seconds, const bool landed_valid, const float target_demuxer_position, const float comparison_timeline_position) {
  (void)active_right_index;
  constexpr float kKeyframeLandingTol = 0.002F;
  const bool keyframe_landing_ok = landed_valid && std::fabs(landed_demuxer_seconds - target_demuxer_position) <= kKeyframeLandingTol;
  if (keyframe_landing_ok) {
    commit_keyframe_navigation_cursor(cursor_, active_right_index, comparison_timeline_position);
    sync_pending_ = true;
    return KeyframeNavigationStatus::None;
  }

  reset_keyframe_navigation_cursor(cursor_);
  sync_pending_ = false;
  pending_steps_.clear();
  return KeyframeNavigationStatus::LandingFailed;
}

}  // namespace playback_keyframe
