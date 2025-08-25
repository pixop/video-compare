#pragma once
#include <string>
#include "core_types.h"
#include "demuxer.h"
#include "side_aware.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mastering_display_metadata.h>
}

class VideoDecoder : public SideAware {
 public:
  explicit VideoDecoder(const Side side,
                        const std::string& decoder_name,
                        const std::string& hw_accel_spec,
                        const AVCodecParameters* codec_parameters,
                        const unsigned peak_luminance_nits,
                        AVDictionary* hwaccel_options,
                        AVDictionary* decoder_options);
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

  AVRational sample_aspect_ratio(const bool reduce = false) const;
  AVRational display_aspect_ratio() const;
  bool is_anamorphic() const;

  int64_t next_pts() const;

  DynamicRange infer_dynamic_range(const std::string& trc_name) const;
  unsigned safe_peak_luminance_nits(const DynamicRange dynamic_range) const;

 private:
  const AVCodec* codec_{};
  AVCodecContext* codec_context_{};

  std::string hw_accel_name_;
  AVPixelFormat hw_pixel_format_;

  int64_t first_pts_;
  int64_t previous_pts_;
  int64_t next_pts_;

  bool trust_decoded_pts_;

  unsigned peak_luminance_nits_;
};
