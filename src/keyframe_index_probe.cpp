#include "keyframe_index_probe.h"

#include <algorithm>
#include <cstdint>
#include <vector>
extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavformat/version.h>
}

namespace playback_keyframe {

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(59, 0, 0)

namespace {

constexpr int kPacketsPerProbe = 96;
constexpr int kKeyframeSeekFlags = 0;

struct ProbeSample {
  bool ok{false};
  int64_t presentation_timestamp{AV_NOPTS_VALUE};
};

static ProbeSample probe_index_timestamp(AVFormatContext* format_context, const int video_stream_index, const int64_t index_timestamp) {
  ProbeSample sample;

  if (format_context == nullptr || video_stream_index < 0 || index_timestamp == AV_NOPTS_VALUE) {
    return sample;
  }

  if (av_seek_frame(format_context, video_stream_index, index_timestamp, kKeyframeSeekFlags) < 0) {
    return sample;
  }

  avformat_flush(format_context);

  AVPacket packet{};

  for (int packets = 0; packets < kPacketsPerProbe; ++packets) {
    if (av_read_frame(format_context, &packet) < 0) {
      break;
    }

    const bool ours = packet.stream_index == video_stream_index;
    const bool key = (packet.flags & AV_PKT_FLAG_KEY) != 0;

    if (!ours || !key) {
      av_packet_unref(&packet);
      continue;
    }

    const int64_t pts = packet.pts;
    const int64_t dts = packet.dts;

    if (dts != AV_NOPTS_VALUE && dts == index_timestamp) {
      if (pts == AV_NOPTS_VALUE) {
        av_packet_unref(&packet);
        return sample;
      }
      sample.ok = true;
      sample.presentation_timestamp = pts;
      av_packet_unref(&packet);
      return sample;
    }

    if (pts != AV_NOPTS_VALUE && pts == index_timestamp) {
      sample.ok = true;
      sample.presentation_timestamp = pts;
      av_packet_unref(&packet);
      return sample;
    }

    av_packet_unref(&packet);
  }

  return sample;
}

static int find_reference_keyframe_index_by_raw_timestamp(const KeyframeIndexSnapshot& snapshot, const int64_t reference_stream_timestamp) {
  if (snapshot.entries.empty()) {
    return -1;
  }

  int best = -1;
  for (size_t i = 0; i < snapshot.entries.size(); ++i) {
    const IndexEntry& entry = snapshot.entries[i];
    if ((entry.flags & AVINDEX_KEYFRAME) == 0) {
      continue;
    }
    if (entry.timestamp <= reference_stream_timestamp) {
      best = static_cast<int>(i);
    } else {
      break;
    }
  }

  return best;
}

static std::vector<size_t> collect_keyframe_indices(const KeyframeIndexSnapshot& snapshot) {
  std::vector<size_t> keyframes;
  keyframes.reserve(snapshot.entries.size());
  for (size_t i = 0; i < snapshot.entries.size(); ++i) {
    if ((snapshot.entries[i].flags & AVINDEX_KEYFRAME) != 0) {
      keyframes.push_back(i);
    }
  }
  return keyframes;
}

static int64_t demuxer_seconds_to_stream_timestamp_local(const float demuxer_seconds, const AVRational& time_base, const float video_start_time_seconds) {
  const double content_seconds = static_cast<double>(demuxer_seconds) - static_cast<double>(video_start_time_seconds);
  const int64_t av_time = static_cast<int64_t>(content_seconds * static_cast<double>(AV_TIME_BASE));
  return av_rescale_q(av_time, AV_TIME_BASE_Q, time_base);
}

static std::vector<size_t> adjacent_probe_indices(const KeyframeIndexSnapshot& snapshot, const int direction, const int reference_index) {
  std::vector<size_t> indices;
  if (reference_index < 0) {
    return indices;
  }

  const int count = static_cast<int>(snapshot.entries.size());

  if (direction < 0) {
    for (int i = reference_index; i >= 0; --i) {
      if ((snapshot.entries[static_cast<size_t>(i)].flags & AVINDEX_KEYFRAME) != 0) {
        indices.push_back(static_cast<size_t>(i));
        if (static_cast<int>(indices.size()) >= 2) {
          break;
        }
      }
    }
    return indices;
  }

  for (int i = reference_index; i < count; ++i) {
    if ((snapshot.entries[static_cast<size_t>(i)].flags & AVINDEX_KEYFRAME) != 0) {
      indices.push_back(static_cast<size_t>(i));
    }
    if (static_cast<int>(indices.size()) >= 3) {
      break;
    }
  }

  return indices;
}

}  // namespace

bool probe_index_entry_presentation(AVFormatContext* format_context, const int video_stream_index, KeyframeIndexSnapshot& snapshot, const size_t entry_index) {
  if (format_context == nullptr || entry_index >= snapshot.entries.size()) {
    return false;
  }

  IndexEntry& entry = snapshot.entries[entry_index];
  if (entry.presentation_timestamp != AV_NOPTS_VALUE) {
    return true;
  }

  const ProbeSample sample = probe_index_timestamp(format_context, video_stream_index, entry.timestamp);
  if (!sample.ok || sample.presentation_timestamp == AV_NOPTS_VALUE) {
    return false;
  }

  entry.presentation_timestamp = sample.presentation_timestamp;
  snapshot.presentation_mapping_verified = true;

  return true;
}

bool prepare_adjacent_keyframe_probe(AVFormatContext* format_context, const int video_stream_index, KeyframeIndexSnapshot& snapshot, const int direction, const float reference_demuxer_seconds) {
  if (format_context == nullptr || format_context->pb == nullptr || !snapshot.seekable || snapshot.entries.empty()) {
    snapshot.seekable = false;
    return false;
  }

  if ((format_context->pb->seekable & AVIO_SEEKABLE_NORMAL) == 0) {
    snapshot.seekable = false;
    return false;
  }

  const std::vector<size_t> keyframes = collect_keyframe_indices(snapshot);
  if (keyframes.empty()) {
    snapshot.seekable = false;
    return false;
  }

  const int64_t reference_presentation_timestamp = demuxer_seconds_to_stream_timestamp_local(reference_demuxer_seconds, snapshot.time_base, snapshot.video_start_time_seconds);
  int reference_index = find_reference_keyframe_index_by_raw_timestamp(snapshot, reference_presentation_timestamp);
  if (reference_index < 0) {
    reference_index = static_cast<int>(keyframes.front());
  }

  std::vector<size_t> probe_targets = adjacent_probe_indices(snapshot, direction, reference_index);

  if (direction > 0 && reference_index > 0) {
    for (int i = reference_index - 1; i >= 0; --i) {
      if ((snapshot.entries[static_cast<size_t>(i)].flags & AVINDEX_KEYFRAME) != 0) {
        const size_t prior = static_cast<size_t>(i);
        if (std::find(probe_targets.begin(), probe_targets.end(), prior) == probe_targets.end()) {
          probe_targets.insert(probe_targets.begin(), prior);
        }
        break;
      }
    }
  }

  if (!snapshot.presentation_mapping_verified) {
    if (std::find(probe_targets.begin(), probe_targets.end(), keyframes.front()) == probe_targets.end()) {
      probe_targets.insert(probe_targets.begin(), keyframes.front());
    }
  }

  for (const size_t index : probe_targets) {
    if (!probe_index_entry_presentation(format_context, video_stream_index, snapshot, index)) {
      return false;
    }
  }

  return snapshot_has_keyframe_index(snapshot);
}

KeyframeIndexSnapshot capture_verified_keyframe_index_snapshot(AVFormatContext* format_context, const int video_stream_index, const float video_start_time_seconds) {
  KeyframeIndexSnapshot snapshot = capture_keyframe_index_snapshot(format_context, video_stream_index, video_start_time_seconds);
  if (!snapshot.seekable || snapshot.entries.empty()) {
    return snapshot;
  }

  const std::vector<size_t> keyframes = collect_keyframe_indices(snapshot);
  if (keyframes.empty()) {
    snapshot.seekable = false;
    return snapshot;
  }

  for (const size_t index : keyframes) {
    if (!probe_index_entry_presentation(format_context, video_stream_index, snapshot, index)) {
      snapshot.presentation_mapping_verified = false;
      snapshot.seekable = false;
      return snapshot;
    }
  }

  return snapshot;
}

#endif

}  // namespace playback_keyframe