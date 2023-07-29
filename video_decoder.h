#pragma once
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
}

class VideoDecoder {
 public:
  explicit VideoDecoder(const std::string& decoder_name, AVCodecParameters* codec_parameters);
  ~VideoDecoder();
  bool send(AVPacket* packet);
  bool receive(AVFrame* frame);
  void flush();
  bool swap_dimensions() const;
  unsigned width() const;
  unsigned height() const;
  AVPixelFormat pixel_format() const;
  AVRational time_base() const;
  const AVCodec* codec() const;
  AVCodecContext* codec_context() const;

 private:
  const AVCodec* codec_{};
  AVCodecContext* codec_context_{};
};
