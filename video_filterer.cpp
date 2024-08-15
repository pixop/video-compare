#include "video_filterer.h"
#include <iostream>
#include <string>
#include <cmath>
#include "ffmpeg.h"
#include "string_utils.h"

VideoFilterer::VideoFilterer(const Demuxer* demuxer, const VideoDecoder* video_decoder, int peak_luminance_nits, const std::string& custom_video_filters, const Demuxer* other_demuxer, const VideoDecoder* other_video_decoder, int other_peak_luminance_nits, const ColorspaceAdaption colorspace_adaption, const float boost_luminance, const bool disable_auto_filters)
    : demuxer_(demuxer),
      video_decoder_(video_decoder),
      width_(video_decoder->width()),
      height_(video_decoder->height()),
      pixel_format_(video_decoder->pixel_format()),
      color_space_(video_decoder->color_space()),
      color_range_(video_decoder->color_range()) {
  std::vector<std::string> filters;

  if (!disable_auto_filters) {
    const bool this_is_interlaced = video_decoder->codec_context()->field_order != AV_FIELD_PROGRESSIVE && video_decoder->codec_context()->field_order != AV_FIELD_UNKNOWN;
    const bool other_is_interlaced = other_video_decoder->codec_context()->field_order != AV_FIELD_PROGRESSIVE && other_video_decoder->codec_context()->field_order != AV_FIELD_UNKNOWN;

    if (this_is_interlaced) {
      filters.push_back("bwdif");
    }

    double this_frame_rate_dbl = av_q2d(demuxer->guess_frame_rate());
    double other_frame_rate_dbl = av_q2d(other_demuxer->guess_frame_rate());

    if (this_is_interlaced) {
      this_frame_rate_dbl *= 2.0;
    }
    if (other_is_interlaced) {
      other_frame_rate_dbl *= 2.0;
    }

    // harmonize the frame rate to the most frames per second
    if (this_frame_rate_dbl < (other_frame_rate_dbl * 0.9995)) {
      filters.push_back(string_sprintf("fps=%.3f", other_frame_rate_dbl));
    }

    // rotation
    if (demuxer->rotation() == 90) {
      filters.push_back("transpose=clock");
    } else if (demuxer->rotation() == 270) {
      filters.push_back("transpose=cclock");
    } else if (demuxer->rotation() == 180) {
      filters.push_back("hflip");
      filters.push_back("vflip");
    } else if (demuxer->rotation() != 0) {
      filters.push_back(string_sprintf("rotate=%d*PI/180", demuxer->rotation()));
    }
  }

  // color space adaption
  if (colorspace_adaption != ColorspaceAdaption::off) {
    const std::string display_primaries = "bt709";
    const std::string display_trc = "iec61966-2-1"; // sRGB

    std::vector<std::string> errors;

    if (video_decoder->color_space() == AVCOL_SPC_UNSPECIFIED) {
      errors.push_back("color space unspecified");
    }
    if (video_decoder->color_primaries() == AVCOL_PRI_UNSPECIFIED) {
      errors.push_back("color primaries unspecified");
    }
    if (video_decoder->color_trc() == AVCOL_TRC_UNSPECIFIED) {
      errors.push_back("transfer characteristics unspecified");
    }
    if (!avfilter_get_by_name("zscale")) {
      errors.push_back("zscale filter missing");
    }

    if (errors.empty()) {
      filters.push_back("format=rgb48");

      float tm_adjustment = (colorspace_adaption == ColorspaceAdaption::tonematch && peak_luminance_nits < other_peak_luminance_nits) ? static_cast<double>(peak_luminance_nits) / other_peak_luminance_nits : 1.0;
      tm_adjustment *= boost_luminance;

      if (std::fabs(tm_adjustment - 1.0F) > 1e-5) {
        filters.push_back(string_sprintf("zscale=t=linear:npl=%d", peak_luminance_nits));
        filters.push_back(string_sprintf("tonemap=clip:param=%.5f", tm_adjustment));
        filters.push_back(string_sprintf("zscale=p=%s:t=%s", display_primaries.c_str(), display_trc.c_str()));
      } else {
        filters.push_back(string_sprintf("zscale=p=%s:t=%s:npl=%d", display_primaries.c_str(), display_trc.c_str(), peak_luminance_nits));
      }
    } else {
      std::cout << string_sprintf("Cannot adapt color space: %s", string_join(errors, ", ").c_str()) << std::endl;
    }
  }

  if (!custom_video_filters.empty()) {
    filters.push_back(custom_video_filters);
  } else if (filters.empty()) {
    filters.push_back("copy");
  }

  filter_description_ = string_join(filters, ",");

  init();
}

VideoFilterer::~VideoFilterer() {
  free();
}

void VideoFilterer::init() {
  filter_graph_ = avfilter_graph_alloc();

  ffmpeg::check(init_filters(video_decoder_->codec_context(), demuxer_->time_base()));
}

void VideoFilterer::free() {
  avfilter_graph_free(&filter_graph_);
}

void VideoFilterer::reinit() {
  free();
  init();
}

int VideoFilterer::init_filters(const AVCodecContext* dec_ctx, const AVRational time_base) {
  AVFilterInOut* outputs = avfilter_inout_alloc();
  AVFilterInOut* inputs = avfilter_inout_alloc();

  int ret = 0;

  if ((outputs == nullptr) || (inputs == nullptr) || (filter_graph_ == nullptr)) {
    ret = AVERROR(ENOMEM);
  } else {
    const int sample_aspect_ratio_den = FFMAX(dec_ctx->sample_aspect_ratio.den, 1);
    const std::string args =
#if (LIBAVFILTER_VERSION_INT < AV_VERSION_INT(10, 1, 100))
        string_sprintf("video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", width_, height_, pixel_format_, time_base.num, time_base.den, dec_ctx->sample_aspect_ratio.num, sample_aspect_ratio_den);
#else
        string_sprintf("video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:colorspace=%d:range=%d", width_, height_, pixel_format_, time_base.num, time_base.den, dec_ctx->sample_aspect_ratio.num, sample_aspect_ratio_den,
                       color_space_, color_range_);
#endif

    // buffer video source: the decoded frames go here
    const AVFilter* buffersrc = avfilter_get_by_name("buffer");

    ret = avfilter_graph_create_filter(&buffersrc_ctx_, buffersrc, "in", args.c_str(), nullptr, filter_graph_);
    if (ret < 0) {
      throw ffmpeg::Error{"Cannot create buffer source"};
    }

    // buffer video sink: terminate the filter chain
    const AVFilter* buffersink = avfilter_get_by_name("buffersink");

    ret = avfilter_graph_create_filter(&buffersink_ctx_, buffersink, "out", nullptr, nullptr, filter_graph_);
    if (ret < 0) {
      throw ffmpeg::Error{"Cannot create buffer sink"};
    }

    outputs->name = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx_;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx_;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    if ((ret = avfilter_graph_parse_ptr(filter_graph_, filter_description_.c_str(), &inputs, &outputs, nullptr)) >= 0) {
      ret = avfilter_graph_config(filter_graph_, nullptr);
    }
  }

  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);

  return ret;
}

bool VideoFilterer::send(AVFrame* decoded_frame) {
  if (decoded_frame != nullptr) {
    bool must_reinit = false;

    if (width_ != decoded_frame->width) {
      width_ = decoded_frame->width;
      must_reinit = true;
    }
    if (height_ != decoded_frame->height) {
      height_ = decoded_frame->height;
      must_reinit = true;
    }
    if (pixel_format_ != decoded_frame->format) {
      if (decoded_frame->format == AV_PIX_FMT_NONE) {
        throw ffmpeg::Error{"Decoded frame with invalid pixel format received"};
      }

      pixel_format_ = static_cast<AVPixelFormat>(decoded_frame->format);
      must_reinit = true;
    }
    if (color_space_ != decoded_frame->colorspace) {
      color_space_ = static_cast<AVColorSpace>(decoded_frame->colorspace);
      must_reinit = true;
    }
    if (color_range_ != decoded_frame->color_range) {
      color_range_ = static_cast<AVColorRange>(decoded_frame->color_range);
      must_reinit = true;
    }

    if (must_reinit) {
      reinit();
    }
  }

  return av_buffersrc_add_frame_flags(buffersrc_ctx_, decoded_frame, AV_BUFFERSRC_FLAG_KEEP_REF) >= 0;
}

void VideoFilterer::close_src() {
  av_buffersrc_close(buffersrc_ctx_, video_decoder_->next_pts(), AV_BUFFERSRC_FLAG_PUSH);
}

bool VideoFilterer::receive(AVFrame* filtered_frame) {
  auto ret = av_buffersink_get_frame_flags(buffersink_ctx_, filtered_frame, 0);

  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
    return false;
  }
  ffmpeg::check(ret);

  // convert PTS and duration to microseconds
  filtered_frame->pts = av_rescale_q(filtered_frame->pts, av_buffersink_get_time_base(buffersink_ctx_), AV_R_MICROSECONDS) - demuxer_->start_time();
  ffmpeg::frame_duration(filtered_frame) = av_rescale_q(ffmpeg::frame_duration(filtered_frame), demuxer_->time_base(), AV_R_MICROSECONDS);

  return true;
}

std::string VideoFilterer::filter_description() const {
  return filter_description_;
}

size_t VideoFilterer::src_width() const {
  return buffersrc_ctx_->outputs[0]->w;
}

size_t VideoFilterer::src_height() const {
  return buffersrc_ctx_->outputs[0]->h;
}

AVPixelFormat VideoFilterer::src_pixel_format() const {
  return static_cast<AVPixelFormat>(buffersrc_ctx_->outputs[0]->format);
}

size_t VideoFilterer::dest_width() const {
  return buffersink_ctx_->inputs[0]->w;
}

size_t VideoFilterer::dest_height() const {
  return buffersink_ctx_->inputs[0]->h;
}

AVPixelFormat VideoFilterer::dest_pixel_format() const {
  return static_cast<AVPixelFormat>(buffersink_ctx_->inputs[0]->format);
}
