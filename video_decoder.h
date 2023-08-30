#pragma once
#include <string>
#include "demuxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

class VideoDecoder {
 public:
  explicit VideoDecoder(const std::string& decoder_name, AVCodecParameters* codec_parameters);
  ~VideoDecoder();
  bool send(AVPacket* packet);
  bool receive(AVFrame* frame, Demuxer *demuxer);
  void flush();
  bool swap_dimensions() const;
  unsigned width() const;
  unsigned height() const;
  AVPixelFormat pixel_format() const;
  AVRational time_base() const;
  const AVCodec* codec() const;
  AVCodecContext* codec_context() const;
  int64_t next_pts() const;

 private:
  const AVCodec* codec_{};
  AVCodecContext* codec_context_{};
  int64_t previous_pts_;
  int64_t next_pts_;
};
