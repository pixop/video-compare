#pragma once
extern "C" {
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
}

class FormatConverter {
 public:
  FormatConverter(size_t src_width, size_t src_height, size_t dest_width, size_t dest_height, AVPixelFormat src_pixel_format, AVPixelFormat dest_pixel_format, AVColorSpace src_color_space, AVColorRange src_color_range);
  ~FormatConverter();

  void init();
  void free();
  void reinit();

  size_t src_width() const;
  size_t src_height() const;
  AVPixelFormat src_pixel_format() const;
  size_t dest_width() const;
  size_t dest_height() const;
  AVPixelFormat dest_pixel_format() const;
  void operator()(AVFrame* src, AVFrame* dst);

 private:
  size_t src_width_;
  size_t src_height_;
  AVPixelFormat src_pixel_format_;

  size_t dest_width_;
  size_t dest_height_;
  AVPixelFormat dest_pixel_format_;

  AVColorSpace src_color_space_;
  AVColorRange src_color_range_;

  SwsContext* conversion_context_{};
};
