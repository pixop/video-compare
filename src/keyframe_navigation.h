#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
extern "C" {
#include <libavutil/frame.h>
}
#include "core_types.h"
#include "playback_keyframe.h"
#include "playback_seek.h"

class Demuxer;

namespace playback_keyframe {

enum class KeyframeNavigationStatus {
  None,
  SeekReady,
  Unavailable,
  AtBoundary,
  SeekSolveFailed,
  LandingFailed,
};

const char* keyframe_navigation_message(KeyframeNavigationStatus status);

struct KeyframeNavigationBegin {
  int direction{0};
  int queue_overflow{0};
};

struct KeyframeNavigationResolve {
  KeyframeNavigationStatus status{KeyframeNavigationStatus::None};
  float seek_relative{0.0F};
  float target_demuxer_position{0.0F};
  float comparison_timeline_position{0.0F};
  SideType candidate_owner{SideType::Left};
};

struct DisplayedDemuxerAnchors {
  float left_demuxer_seconds{0.0F};
  bool left_valid{false};
  float right_demuxer_seconds{0.0F};
  bool right_valid{false};
};

template <typename FramePtr>
const AVFrame* displayed_frame_at_offset(const std::deque<FramePtr>& frames, const int frame_offset) {
  if (frame_offset < 0 || frames.empty()) {
    return nullptr;
  }
  const size_t display_index = std::min(static_cast<size_t>(frame_offset), frames.size() - 1);
  return frames[display_index].get();
}

DisplayedDemuxerAnchors displayed_demuxer_anchors(const AVFrame* left_frame, const Demuxer& left_demuxer, const AVFrame* right_frame, const Demuxer& right_demuxer);

class KeyframeNavigation {
 public:
  void reset_session();

  KeyframeNavigationBegin begin_frame(const int keyframe_navigation_delta, const bool in_sync_when_pending);

  KeyframeNavigationResolve resolve_step(const int step_direction,
                                         Demuxer& left_demuxer,
                                         Demuxer& right_demuxer,
                                         const playback_seek::SeekRequest& seek_request,
                                         const size_t active_right_index,
                                         const float displayed_left_demuxer_seconds,
                                         const bool displayed_left_valid,
                                         const float displayed_right_demuxer_seconds,
                                         const bool displayed_right_valid);

  KeyframeNavigationStatus verify_landing(const size_t active_right_index, const float landed_demuxer_seconds, const bool landed_valid, const float target_demuxer_position, const float comparison_timeline_position);

  KeyframeNavigationStatus verify_landing_from_frames(const KeyframeNavigationResolve& resolve,
                                                      const size_t active_right_index,
                                                      const AVFrame* left_frame,
                                                      const Demuxer& left_demuxer,
                                                      const AVFrame* right_frame,
                                                      const Demuxer& right_demuxer);

  const KeyframeNavigationCursor& cursor() const { return cursor_; }
  bool has_pending_steps() const { return !pending_steps_.empty(); }

 private:
  static constexpr size_t kMaxPendingSteps = 32;

  void enqueue_steps(const int delta);

  KeyframeNavigationCursor cursor_;
  bool sync_pending_{false};
  std::deque<int> pending_steps_;
};

}  // namespace playback_keyframe