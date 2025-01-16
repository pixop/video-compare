#pragma once
#include <string>
#include "side_aware.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/display.h>
}

class Demuxer : public SideAware {
 public:
  explicit Demuxer(const Side side, const std::string& demuxer_name, const std::string& file_name, AVDictionary* demuxer_options, const AVDictionary* decoder_options);
  ~Demuxer();

  AVCodecParameters* video_codec_parameters();

  int video_stream_index() const;

  AVRational time_base() const;
  int64_t duration() const;
  int64_t start_time() const;

  int rotation() const;

  AVRational guess_frame_rate(AVFrame* frame = nullptr) const;

  bool operator()(AVPacket& packet);
  bool seek(float position, bool backward);

  std::string format_name();
  int64_t file_size();
  int64_t bit_rate();

 private:
  AVFormatContext* format_context_{};
  int video_stream_index_{};
};
