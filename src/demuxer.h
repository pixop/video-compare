#pragma once
#include <string>
#include "keyframe_index.h"
#include "side_aware.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/display.h>
}

class Demuxer : public SideAware {
 public:
  explicit Demuxer(const Side& side, const std::string& demuxer_name, const std::string& file_name, AVDictionary* demuxer_options, const AVDictionary* decoder_options);
  ~Demuxer();

  AVCodecParameters* video_codec_parameters();

  int video_stream_index() const;

  AVRational time_base() const;
  int64_t duration() const;
  int64_t video_start_time() const;
  float video_start_time_seconds() const;

  int rotation() const;

  AVRational guess_frame_rate(AVFrame* frame = nullptr) const;
  AVRational sample_aspect_ratio(AVFrame* frame = nullptr) const;

  bool operator()(AVPacket& packet);
  bool seek(float position, bool backward);
  bool seek_keyframe_owner(const float key_demuxer_seconds);
  bool seek_keyframe_preroll(const float target_presentation_seconds);

  playback_keyframe::AdjacentKeyframe adjacent_keyframe(const int direction, const float reference_demuxer_seconds);
  bool index_navigation_supported();
  bool keyframe_navigation_seekable() const { return keyframe_navigation_seekable_; }

  std::string format_name();
  int64_t file_size();
  int64_t bit_rate();

 private:
  bool ensure_keyframe_index_initialized();
  bool ensure_probe_format_context();
  void close_probe_format_context();
  bool probe_context_matches_live() const;

  int64_t container_start_time() const;

  std::string demuxer_name_;
  std::string file_name_;

  AVFormatContext* format_context_{};
  AVFormatContext* probe_format_context_{};
  AVDictionary* demuxer_options_copy_{nullptr};
  int video_stream_index_{};
  int probe_video_stream_index_{-1};
  bool probe_context_open_{false};
  bool keyframe_navigation_seekable_{false};
  bool keyframe_index_init_attempted_{false};
  playback_keyframe::KeyframeIndexSnapshot keyframe_index_snapshot_{};
};