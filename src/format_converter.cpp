#include "format_converter.h"
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include "ffmpeg.h"
#include "frame_metadata.h"
extern "C" {
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
}

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
                                 const ConversionFit conversion_fit,
                                 const Side& side,
                                 const int flags)
    : SideAware(side),
      src_width_{src_width},
      src_height_{src_height},
      src_pixel_format_{src_pixel_format},
      dest_width_{dest_width},
      dest_height_{dest_height},
      dest_pixel_format_{dest_pixel_format},
      conversion_fit_{conversion_fit},
      src_color_space_{src_color_space},
      src_color_range_{src_color_range},
      active_flags_(flags),
      pending_flags_(active_flags_) {
  ScopedLogSide scoped_log_side(side);

  ensure_source_fits_canvas();
  init();
}

FormatConverter::~FormatConverter() {
  free();
  av_freep(&native_scratch_);

  native_scratch_linesize_ = 0;
  native_scratch_bytes_ = 0;
}

void FormatConverter::ensure_source_fits_canvas() const {
  if (conversion_fit_ != ConversionFit::Native) {
    return;
  }
  if (src_width_ > dest_width_ || src_height_ > dest_height_) {
    throw std::runtime_error("Filtered frame " + std::to_string(src_width_) + "x" + std::to_string(src_height_) + " does not fit conversion canvas " + std::to_string(dest_width_) + "x" + std::to_string(dest_height_));
  }
}

void FormatConverter::ensure_native_scratch(const int width, const int height) {
  int linesize[4] = {0, 0, 0, 0};

  if (av_image_fill_linesizes(linesize, dest_pixel_format_, width) < 0) {
    throw ffmpeg::Error{"Could not compute native conversion scratch linesize"};
  }

  const int aligned_linesize = (linesize[0] + 63) & ~63;
  const size_t bytes = static_cast<size_t>(aligned_linesize) * static_cast<size_t>(height);

  if (bytes > native_scratch_bytes_) {
    uint8_t* scratch = static_cast<uint8_t*>(av_malloc(bytes));

    if (scratch == nullptr) {
      throw ffmpeg::Error{"Could not allocate native conversion scratch"};
    }

    av_freep(&native_scratch_);

    native_scratch_ = scratch;
    native_scratch_bytes_ = bytes;
  }

  native_scratch_linesize_ = aligned_linesize;
}

int FormatConverter::packed_dest_bytes_per_pixel() const {
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(dest_pixel_format_);
  if (desc == nullptr) {
    throw ffmpeg::Error{"Unknown destination pixel format"};
  }

  const bool packed_rgb = ((desc->flags & AV_PIX_FMT_FLAG_RGB) != 0) && ((desc->flags & AV_PIX_FMT_FLAG_PLANAR) == 0) && ((desc->flags & AV_PIX_FMT_FLAG_BITSTREAM) == 0);
  if (!packed_rgb || av_pix_fmt_count_planes(dest_pixel_format_) != 1) {
    throw ffmpeg::Error{"Destination pixel format is not packed byte RGB"};
  }

  const int bits = av_get_padded_bits_per_pixel(desc);
  if (bits <= 0 || (bits % 8) != 0) {
    throw ffmpeg::Error{"Destination pixel format is not packed byte RGB"};
  }

  return bits / 8;
}

void FormatConverter::init() {
  const int sws_dest_w = (conversion_fit_ == ConversionFit::Native) ? static_cast<int>(src_width_) : static_cast<int>(dest_width_);
  const int sws_dest_h = (conversion_fit_ == ConversionFit::Native) ? static_cast<int>(src_height_) : static_cast<int>(dest_height_);

  conversion_context_ = sws_getContext(
      // Source
      static_cast<int>(src_width()), static_cast<int>(src_height()), src_pixel_format(),
      // Destination
      sws_dest_w, sws_dest_h, dest_pixel_format(),
      // Filters
      active_flags_, nullptr, nullptr, nullptr);

  if (conversion_context_ == nullptr) {
    throw ffmpeg::Error{"Could not initialize format conversion context"};
  }

  const int sws_color_space = get_sws_colorspace(src_color_space_);
  const int sws_color_range = get_sws_range(src_color_range_);
  const int* yuv2rgb_coeffs = sws_getCoefficients(sws_color_space);

  sws_setColorspaceDetails(conversion_context_, yuv2rgb_coeffs, sws_color_range, yuv2rgb_coeffs, sws_color_range, 0, FIXED_1_0, FIXED_1_0);
}

void FormatConverter::free() {
  sws_freeContext(conversion_context_);
  conversion_context_ = nullptr;
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
    ensure_source_fits_canvas();
    reinit();
  }

  FrameMetadata::set_original_dimensions(dst, src->width, src->height);
  FrameMetadata::set_key(dst, FrameMetadata::make_frame_key(src));

  uint8_t* dst_planes[4] = {dst->data[0], dst->data[1], dst->data[2], dst->data[3]};
  int dst_linesizes[4] = {dst->linesize[0], dst->linesize[1], dst->linesize[2], dst->linesize[3]};

  bool used_scratch = false;

  if (conversion_fit_ == ConversionFit::Native) {
    const ContentRect content = content_rect_in_canvas(conversion_fit_, static_cast<int>(dest_width_), static_cast<int>(dest_height_), static_cast<int>(src_width_), static_cast<int>(src_height_));
    const bool has_padding = (content.x != 0) || (content.y != 0) || (content.width != static_cast<int>(dest_width_)) || (content.height != static_cast<int>(dest_height_));

    if (has_padding) {
      if (dst->data[0] != nullptr && dst->linesize[0] > 0) {
        std::memset(dst->data[0], 0, static_cast<size_t>(dst->linesize[0]) * dest_height_);
      }

      if (content.x != 0) {
        // Avoid passing an interior, potentially unaligned packed-RGB
        // destination pointer to sws_scale. Convert to aligned scratch, then blit.
        const int bpp = packed_dest_bytes_per_pixel();

        ensure_native_scratch(content.width, content.height);

        uint8_t* scratch_planes[4] = {native_scratch_, nullptr, nullptr, nullptr};
        int scratch_linesizes[4] = {native_scratch_linesize_, 0, 0, 0};

        sws_scale(conversion_context_, src->data, src->linesize, 0, static_cast<int>(src_height_), scratch_planes, scratch_linesizes);

        const int row_bytes = content.width * bpp;
        for (int y = 0; y < content.height; ++y) {
          std::memcpy(dst->data[0] + static_cast<ptrdiff_t>(content.y + y) * dst->linesize[0] + static_cast<ptrdiff_t>(content.x) * bpp, native_scratch_ + static_cast<ptrdiff_t>(y) * native_scratch_linesize_,
                      static_cast<size_t>(row_bytes));
        }

        used_scratch = true;
      } else if (content.y != 0) {
        dst_planes[0] = dst->data[0] + static_cast<ptrdiff_t>(content.y) * dst->linesize[0];
        dst_planes[1] = nullptr;
        dst_planes[2] = nullptr;
        dst_planes[3] = nullptr;
      }
    }
  }

  if (!used_scratch) {
    sws_scale(conversion_context_,
              // Source
              src->data, src->linesize, 0, static_cast<int>(src_height_),
              // Destination
              dst_planes, dst_linesizes);
  }

  dst->format = dest_pixel_format();
  dst->width = static_cast<int>(dest_width());
  dst->height = static_cast<int>(dest_height());
}
