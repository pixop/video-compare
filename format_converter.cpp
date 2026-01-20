#include "format_converter.h"
#include <iostream>
#include "ffmpeg.h"

static constexpr int FIXED_1_0 = (1 << 16);

inline int get_sws_colorspace(const AVColorSpace color_space) {
  switch (color_space) {
    case AVCOL_SPC_BT709:
      return SWS_CS_ITU709;
    case AVCOL_SPC_FCC:
      return SWS_CS_FCC;
    case AVCOL_SPC_SMPTE170M:
      return SWS_CS_SMPTE170M;
    case AVCOL_SPC_SMPTE240M:
      return SWS_CS_SMPTE240M;
    case AVCOL_SPC_BT2020_CL:
    case AVCOL_SPC_BT2020_NCL:
      return SWS_CS_BT2020;
    default:
      break;
  }

  return SWS_CS_ITU601;
}

inline int get_sws_range(const AVColorRange color_range) {
  return color_range == AVCOL_RANGE_JPEG ? 1 : 0;
}

FormatConverter::FormatConverter(const size_t src_width,
                                 const size_t src_height,
                                 const size_t dest_width,
                                 const size_t dest_height,
                                 const AVPixelFormat src_pixel_format,
                                 const AVPixelFormat dest_pixel_format,
                                 const AVColorSpace src_color_space,
                                 const AVColorRange src_color_range,
                                 const Side& side,
                                 const int flags)
    : SideAware(side),
      src_width_{src_width},
      src_height_{src_height},
      src_pixel_format_{src_pixel_format},
      dest_width_{dest_width},
      dest_height_{dest_height},
      dest_pixel_format_{dest_pixel_format},
      src_color_space_{src_color_space},
      src_color_range_{src_color_range},
      active_flags_(flags),
      pending_flags_(active_flags_) {
  ScopedLogSide scoped_log_side(side);

  init();
}

FormatConverter::~FormatConverter() {
  free();
}

void FormatConverter::init() {
  conversion_context_ = sws_getContext(
      // Source
      src_width(), src_height(), src_pixel_format(),
      // Destination
      dest_width(), dest_height(), dest_pixel_format(),
      // Filters
      active_flags_, nullptr, nullptr, nullptr);

  // set colorspace details
  const int sws_color_space = get_sws_colorspace(src_color_space_);
  const int sws_color_range = get_sws_range(src_color_range_);
  const int* yuv2rgb_coeffs = sws_getCoefficients(sws_color_space);

  sws_setColorspaceDetails(conversion_context_, yuv2rgb_coeffs, sws_color_range, yuv2rgb_coeffs, sws_color_range, 0, FIXED_1_0, FIXED_1_0);
}

void FormatConverter::free() {
  sws_freeContext(conversion_context_);
}

void FormatConverter::reinit() {
  free();
  init();
}

size_t FormatConverter::src_width() const {
  return src_width_;
}

size_t FormatConverter::src_height() const {
  return src_height_;
}

AVPixelFormat FormatConverter::src_pixel_format() const {
  return src_pixel_format_;
}

size_t FormatConverter::dest_width() const {
  return dest_width_;
}

size_t FormatConverter::dest_height() const {
  return dest_height_;
}

AVPixelFormat FormatConverter::dest_pixel_format() const {
  return dest_pixel_format_;
}

void FormatConverter::set_pending_flags(const int flags) {
  pending_flags_ = flags;
}

void FormatConverter::operator()(AVFrame* src, AVFrame* dst) {
  bool must_reinit = false;

  if (src_width_ != static_cast<size_t>(src->width)) {
    src_width_ = src->width;
    must_reinit = true;
  }
  if (src_height_ != static_cast<size_t>(src->height)) {
    src_height_ = src->height;
    must_reinit = true;
  }
  if (src_pixel_format_ != src->format) {
    if (src->format == AV_PIX_FMT_NONE) {
      throw ffmpeg::Error{"Format converter got a source frame with invalid pixel format"};
    }

    src_pixel_format_ = static_cast<AVPixelFormat>(src->format);
    must_reinit = true;
  }
  if (src_color_space_ != src->colorspace) {
    src_color_space_ = src->colorspace;
    must_reinit = true;
  }
  if (src_color_range_ != src->color_range) {
    src_color_range_ = src->color_range;
    must_reinit = true;
  }
  if (pending_flags_ != active_flags_) {
    active_flags_ = pending_flags_;
    must_reinit = true;
  }

  if (must_reinit) {
    reinit();
  }

  av_dict_set(&dst->metadata, "original_width", std::to_string(src->width).c_str(), 0);
  av_dict_set(&dst->metadata, "original_height", std::to_string(src->height).c_str(), 0);

  sws_scale(conversion_context_,
            // Source
            src->data, src->linesize, 0, src_height_,
            // Destination
            dst->data, dst->linesize);

  dst->format = dest_pixel_format();
  dst->width = dest_width();
  dst->height = dest_height();
}
