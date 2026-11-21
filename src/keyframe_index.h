#pragma once

#include <cstdint>
#include <vector>
extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavformat/version.h>
#include <libavutil/rational.h>
#include <libavutil/avutil.h>
}

namespace playback_keyframe {

enum class AdjacentKeyframeStatus { Ok, Unavailable, AtBoundary };

struct AdjacentKeyframe {
  AdjacentKeyframeStatus status{AdjacentKeyframeStatus::Unavailable};
  float demuxer_position_seconds{0.0F};
  int64_t stream_timestamp{0};
};

struct IndexEntry {
  int64_t timestamp{0};
  int flags{0};
  int64_t presentation_timestamp{AV_NOPTS_VALUE};
};

struct KeyframeIndexSnapshot {
  bool seekable{false};
  bool presentation_mapping_verified{false};
  AVRational time_base{0, 1};
  float video_start_time_seconds{0.0F};
  std::vector<IndexEntry> entries;
};

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(59, 0, 0)
inline KeyframeIndexSnapshot capture_keyframe_index_snapshot(AVFormatContext* format_context, const int video_stream_index, const float video_start_time_seconds) {
  KeyframeIndexSnapshot snapshot;
  snapshot.video_start_time_seconds = video_start_time_seconds;

  if (format_context == nullptr || format_context->pb == nullptr || (format_context->pb->seekable & AVIO_SEEKABLE_NORMAL) == 0) {
    return snapshot;
  }

  if (video_stream_index < 0 || static_cast<unsigned int>(video_stream_index) >= format_context->nb_streams) {
    return snapshot;
  }

  AVStream* stream = format_context->streams[video_stream_index];
  if (stream == nullptr) {
    return snapshot;
  }

  snapshot.time_base = stream->time_base;
  if (snapshot.time_base.num <= 0 || snapshot.time_base.den <= 0) {
    return snapshot;
  }

  snapshot.seekable = true;

  const int count = avformat_index_get_entries_count(stream);
  if (count <= 0) {
    snapshot.seekable = false;
    return snapshot;
  }

  snapshot.entries.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    const AVIndexEntry* entry = avformat_index_get_entry(stream, i);
    if (entry == nullptr) {
      continue;
    }
    snapshot.entries.push_back(IndexEntry{entry->timestamp, entry->flags});
  }

  if (snapshot.entries.empty()) {
    snapshot.seekable = false;
  }

  return snapshot;
}
#endif

inline bool raw_index_has_keyframes(const KeyframeIndexSnapshot& snapshot) {
  if (!snapshot.seekable || snapshot.entries.empty()) {
    return false;
  }

  for (const IndexEntry& entry : snapshot.entries) {
    if ((entry.flags & AVINDEX_KEYFRAME) != 0) {
      return true;
    }
  }

  return false;
}

inline bool snapshot_has_keyframe_index(const KeyframeIndexSnapshot& snapshot) {
  if (!snapshot.presentation_mapping_verified || !raw_index_has_keyframes(snapshot)) {
    return false;
  }

  return true;
}

}  // namespace playback_keyframe
