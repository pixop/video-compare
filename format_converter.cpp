#include "format_converter.h"
#include <iostream>
#include "ffmpeg.h"

FormatConverter::FormatConverter(size_t src_width, size_t src_height, size_t dest_width, size_t dest_height, AVPixelFormat input_pixel_format, AVPixelFormat output_pixel_format)
    : src_width_{src_width}, src_height_{src_height}, input_pixel_format_{input_pixel_format}, dest_width_{dest_width}, dest_height_{dest_height}, output_pixel_format_{output_pixel_format} {
  init();
}

FormatConverter::~FormatConverter() {
  free();
}

void FormatConverter::init() {
  conversion_context_ = sws_getContext(
      // Source
      src_width(), src_height(), input_pixel_format(),
      // Destination
      dest_width(), dest_height(), output_pixel_format(),
      // Filters
      SWS_BICUBIC, nullptr, nullptr, nullptr);
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

AVPixelFormat FormatConverter::input_pixel_format() const {
  return input_pixel_format_;
}

size_t FormatConverter::dest_width() const {
  return dest_width_;
}

size_t FormatConverter::dest_height() const {
  return dest_height_;
}

AVPixelFormat FormatConverter::output_pixel_format() const {
  return output_pixel_format_;
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
  if (input_pixel_format_ != src->format) {
    if (src->format == AV_PIX_FMT_NONE) {
      throw ffmpeg::Error{"Format converter got a source frame with invalid pixel format"};
    }

    input_pixel_format_ = static_cast<AVPixelFormat>(src->format);
    must_reinit = true;
  }

  if (must_reinit) {
    reinit();
  }

  sws_scale(conversion_context_,
            // Source
            src->data, src->linesize, 0, src_height_,
            // Destination
            dst->data, dst->linesize);
}
