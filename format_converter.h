#pragma once
extern "C" {
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
}

class FormatConverter {
 public:
  FormatConverter(size_t src_width, size_t src_height, size_t dest_width, size_t dest_height, AVPixelFormat input_pixel_format, AVPixelFormat output_pixel_format);
  ~FormatConverter();

  void init();
  void free();
  void reinit();

  size_t src_width() const;
  size_t src_height() const;
  AVPixelFormat input_pixel_format() const;
  size_t dest_width() const;
  size_t dest_height() const;
  AVPixelFormat output_pixel_format() const;
  void operator()(AVFrame* src, AVFrame* dst);

 private:
  size_t src_width_;
  size_t src_height_;
  AVPixelFormat input_pixel_format_;
  size_t dest_width_;
  size_t dest_height_;
  AVPixelFormat output_pixel_format_;

  SwsContext* conversion_context_{};
};
