#pragma once
#include <string>
#include "demuxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

class VideoDecoder {
 public:
  explicit VideoDecoder(const std::string& decoder_name, const std::string& hw_accel_spec, AVCodecParameters* codec_parameters);
  ~VideoDecoder();

  const AVCodec* codec() const;
  AVCodecContext* codec_context() const;

  bool is_hw_accelerated() const;
  std::string hw_accel_name() const;

  bool send(AVPacket* packet);
  bool receive(AVFrame* frame, Demuxer* demuxer);
  void flush();
  bool swap_dimensions() const;
  unsigned width() const;
  unsigned height() const;
  AVPixelFormat pixel_format() const;
  AVPixelFormat hw_pixel_format() const;
  AVColorRange color_range() const;
  AVColorSpace color_space() const;
  AVColorPrimaries color_primaries() const;
  AVColorTransferCharacteristic color_trc() const;
  AVRational time_base() const;

  int64_t next_pts() const;

 private:
  const AVCodec* codec_{};
  AVCodecContext* codec_context_{};

  std::string hw_accel_name_;
  AVPixelFormat hw_pixel_format_;

  int64_t previous_pts_;
  int64_t next_pts_;
};
