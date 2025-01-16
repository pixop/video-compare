#pragma once
#include "side_aware.h"
extern "C" {
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
}

class FormatConverter : public SideAware {
 public:
  FormatConverter(const size_t src_width,
                  const size_t src_height,
                  const size_t dest_width,
                  const size_t dest_height,
                  const AVPixelFormat src_pixel_format,
                  const AVPixelFormat dest_pixel_format,
                  const AVColorSpace src_color_space,
                  const AVColorRange src_color_range,
                  const Side side = NONE,
                  const int flags = SWS_FAST_BILINEAR);
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

  void set_pending_flags(const int flags);

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

  int active_flags_;
  int pending_flags_;

  SwsContext* conversion_context_{};
};
