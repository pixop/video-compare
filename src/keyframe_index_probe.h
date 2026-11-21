#pragma once

#include "keyframe_index.h"
extern "C" {
#include <libavformat/avformat.h>
}

namespace playback_keyframe {

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(59, 0, 0)

bool probe_index_entry_presentation(AVFormatContext* format_context, const int video_stream_index, KeyframeIndexSnapshot& snapshot, const size_t entry_index);

// Lazily probe only the index entries needed to resolve adjacent keyframe navigation
// (reference keyframe, candidate neighbor, and one offset sample when unverified).
// Restores demuxer read position to restore_demuxer_seconds on success; marks snapshot
// unavailable on failure.
bool prepare_adjacent_keyframe_probe(AVFormatContext* format_context, const int video_stream_index, KeyframeIndexSnapshot& snapshot, const int direction, const float reference_demuxer_seconds);

KeyframeIndexSnapshot capture_verified_keyframe_index_snapshot(AVFormatContext* format_context, const int video_stream_index, const float video_start_time_seconds);

#endif

// Even a uniform offset across sampled keyframes is not proof that the next
// indexed keyframe has the same DTS-to-PTS offset. Only use verified PTS.
inline int64_t index_entry_presentation_timestamp(const KeyframeIndexSnapshot&, const IndexEntry& entry) {
  return entry.presentation_timestamp;
}

}  // namespace playback_keyframe