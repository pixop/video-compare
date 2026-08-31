#pragma once
#include <atomic>
#include "conversion_geometry.h"
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
                  const ConversionFit conversion_fit = ConversionFit::Stretch,
                  const Side& side = NONE,
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
  void ensure_source_fits_canvas() const;
  int packed_dest_bytes_per_pixel() const;
  void ensure_native_scratch(const int width, const int height);

  size_t src_width_;
  size_t src_height_;
  AVPixelFormat src_pixel_format_;

  const size_t dest_width_;
  const size_t dest_height_;
  const AVPixelFormat dest_pixel_format_;
  const ConversionFit conversion_fit_;

  AVColorSpace src_color_space_;
  AVColorRange src_color_range_;

  int active_flags_;
  std::atomic<int> pending_flags_;

  SwsContext* conversion_context_{};
  uint8_t* native_scratch_{};
  int native_scratch_linesize_{};
  size_t native_scratch_bytes_{};
};
