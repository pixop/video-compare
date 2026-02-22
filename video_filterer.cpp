#include "video_filterer.h"
#include <cmath>
#include <iostream>
#include <string>
#include "ffmpeg.h"
#include "string_utils.h"
#include "video_filter_context.h"

static constexpr char VIDEO_FILTER_GROUP_DELIMITER = '|';

static std::string crop_filter(const CropRect& rect) {
  return string_sprintf("crop=%d:%d:%d:%d", rect.w, rect.h, rect.x, rect.y);
}

static std::string compose_filters(const std::string& pre_filters, const std::string& post_filters, const CropRect& rect, const bool crop_enabled) {
  std::vector<std::string> filter_groups;

  if (!pre_filters.empty()) {
    filter_groups.push_back(pre_filters);
  }

  if (crop_enabled) {
    filter_groups.push_back(crop_filter(rect));
  }

  if (!post_filters.empty()) {
    filter_groups.push_back(post_filters);
  }

  if (filter_groups.empty()) {
    return "copy";
  }

  return string_join(filter_groups, ",");
}

static unsigned get_content_light_level_or_zero(const AVFrame* frame) {
  AVFrameSideData* frame_side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);

  if (frame_side_data != nullptr && static_cast<size_t>(frame_side_data->size) >= sizeof(AVContentLightMetadata)) {
    AVContentLightMetadata* cll_metadata = reinterpret_cast<AVContentLightMetadata*>(frame_side_data->data);

    return cll_metadata->MaxCLL;
  }

  return UNSET_PEAK_LUMINANCE;
}

VideoFilterer::VideoFilterer(const Side& side,
                             const Demuxer* demuxer,
                             const VideoDecoder* video_decoder,
                             const ToneMapping tone_mapping_mode,
                             const float boost_tone,
                             const std::string& custom_video_filters,
                             const std::string& custom_color_space,
                             const std::string& custom_color_range,
                             const std::string& custom_color_primaries,
                             const std::string& custom_color_trc,
                             const VideoFilterContext* video_filter_context,
                             const bool disable_auto_filters)
    : SideAware(side),
      demuxer_(demuxer),
      video_decoder_(video_decoder),
      tone_mapping_mode_(tone_mapping_mode),
      width_(video_decoder->width()),
      height_(video_decoder->height()),
      pixel_format_(video_decoder->pixel_format()),
      color_space_(video_decoder->color_space()),
      color_range_(video_decoder->color_range()),
      sample_aspect_ratio_(video_decoder->sample_aspect_ratio(demuxer_, true)),
      time_base_(demuxer_->time_base()) {
  ScopedLogSide scoped_log_side(side);

  std::vector<std::string> pre_filters;
  std::vector<std::string> post_filters;

  // up to two filter groups are allowed ("pre" and "post"), if only a single group is specified it is assigned to the "post" group
  const std::vector<std::string> custom_filter_groups = string_split(custom_video_filters, VIDEO_FILTER_GROUP_DELIMITER);

  std::string custom_pre_filters, custom_post_filters;

  if (custom_filter_groups.size() == 2 || (custom_filter_groups.size() == 1 && custom_video_filters.back() == VIDEO_FILTER_GROUP_DELIMITER)) {
    custom_pre_filters = custom_filter_groups[0];

    if (custom_filter_groups.size() > 1) {
      custom_post_filters = custom_filter_groups[1];
    }
  } else if (custom_filter_groups.size() == 1) {
    custom_post_filters = custom_filter_groups[0];
  } else if (custom_filter_groups.size() > 2) {
    throw std::runtime_error("No more than 2 filter groups supported");
  }

  // custom pre-filtering can for example be used to override the color space, primaries and trc settings in case of incorrect metadata before any tone-mapping is performed
  // for example: 'setparams=colorspace=bt709|' (if not post-filtering is desired)
  if (!custom_pre_filters.empty()) {
    pre_filters.push_back(custom_pre_filters);
  }

  if (!disable_auto_filters) {
    // deinterlacing
    const bool this_is_interlaced = video_decoder->codec_context()->field_order != AV_FIELD_PROGRESSIVE && video_decoder->codec_context()->field_order != AV_FIELD_UNKNOWN;

    if (this_is_interlaced) {
      pre_filters.push_back("bwdif");
    }

    double this_frame_rate_dbl = av_q2d(demuxer->guess_frame_rate());
    double max_other_frame_rate_dbl = video_filter_context->get_max_frame_rate_excluding(side);

    if (this_is_interlaced) {
      this_frame_rate_dbl *= 2.0;
    }

    // stretch to display aspect ratio
    if (video_decoder->is_anamorphic(demuxer_)) {
      if (sample_aspect_ratio_.num > sample_aspect_ratio_.den) {
        pre_filters.push_back("scale=iw*sar:ih");
      } else {
        pre_filters.push_back("scale=iw:ih/sar");
      }
    }

    // harmonize the frame rate to the most frames per second
    if (this_frame_rate_dbl < (max_other_frame_rate_dbl * 0.9995)) {
      pre_filters.push_back(string_sprintf("fps=%.3f", max_other_frame_rate_dbl));
    }

    // rotation
    if (demuxer->rotation() == 90) {
      pre_filters.push_back("transpose=clock");
    } else if (demuxer->rotation() == 270) {
      pre_filters.push_back("transpose=cclock");
    } else if (demuxer->rotation() == 180) {
      pre_filters.push_back("hflip");
      pre_filters.push_back("vflip");
    } else if (demuxer->rotation() != 0) {
      pre_filters.push_back(string_sprintf("rotate=%d*PI/180", demuxer->rotation()));
    }
  }

  dynamic_range_ = video_decoder->infer_dynamic_range(custom_color_trc);
  const bool is_hdr_trc = dynamic_range_ != DynamicRange::Standard;
  const bool must_tonemap = tone_mapping_mode == ToneMapping::FullRange || tone_mapping_mode == ToneMapping::Relative || (tone_mapping_mode == ToneMapping::Auto && is_hdr_trc);

  // resolve initial peak luminance
  peak_luminance_nits_ = video_decoder->safe_peak_luminance_nits(dynamic_range_);

  if (tone_mapping_mode == ToneMapping::Auto && is_hdr_trc) {
    const char* msg;

    if (dynamic_range_ == DynamicRange::PQ) {
      msg = "PQ / SMPTE ST 2084 transfer characteristics (smpte2084)";
    } else if (dynamic_range_ == DynamicRange::HLG) {
      msg = "Hybrid log–gamma transfer characteristics (arib-std-b67)";
    } else {
      msg = "Unknown transfer characteristics";
    }

    log_info(string_sprintf("%s applied; performing HDR color space conversion at an initial %d nits.", msg, peak_luminance_nits_).c_str());
  }

  // set color space and range (+ primaries and TRC if tone-mapping is required) to limited range Rec. 709 if metadata is unspecified or pass any user-provided values
  if (!disable_auto_filters || must_tonemap || !custom_color_space.empty() || !custom_color_range.empty() || !custom_color_primaries.empty() || !custom_color_trc.empty()) {
    std::vector<std::string> notes, setparams_options;

    if ((video_decoder->color_space() == AVCOL_SPC_UNSPECIFIED) || !custom_color_space.empty()) {
      if (custom_color_space.empty()) {
        notes.push_back("'Color space' (colorspace)");
      }
      setparams_options.push_back("colorspace=" + (custom_color_space.empty() ? "bt709" : custom_color_space));
    }
    if ((video_decoder->color_range() == AVCOL_RANGE_UNSPECIFIED) || !custom_color_range.empty()) {
      if (custom_color_range.empty()) {
        notes.push_back("'Color range' (range)");
      }
      setparams_options.push_back("range=" + (custom_color_range.empty() ? "tv" : custom_color_range));
    }
    if ((must_tonemap && video_decoder->color_primaries() == AVCOL_PRI_UNSPECIFIED) || !custom_color_primaries.empty()) {
      if (custom_color_primaries.empty()) {
        notes.push_back("'Color primaries' (color_primaries)");
      }
      setparams_options.push_back("color_primaries=" + (custom_color_primaries.empty() ? "bt709" : custom_color_primaries));
    }
    if ((must_tonemap && video_decoder->color_trc() == AVCOL_TRC_UNSPECIFIED) || !custom_color_trc.empty()) {
      if (custom_color_trc.empty()) {
        notes.push_back("'Transfer characteristics' (color_trc)");
      }
      setparams_options.push_back("color_trc=" + (custom_color_trc.empty() ? "bt709" : custom_color_trc));
    }

    if (!notes.empty()) {
      log_warning(string_sprintf("Metadata is missing for %s; assuming limited range Rec. 709. It is recommended to manually set the missing properties to their correct values.", string_join(notes, ", ").c_str()));
    }
    if (!setparams_options.empty()) {
      pre_filters.push_back(string_sprintf("setparams=%s", string_join(setparams_options, ":").c_str()));
    }
  }

  // tone-mapping
  if (must_tonemap) {
    const std::string display_primaries = "bt709";
    const std::string display_trc = "iec61966-2-1";  // sRGB

    std::vector<std::string> warnings;

    if (!avfilter_get_by_name("zscale")) {
      warnings.push_back("zscale filter missing in libavfilter build");
    }

    if (warnings.empty()) {
      const unsigned other_peak_luminance_nits = video_filter_context->get_max_peak_luminance_excluding(side);

      float tone_adjustment = (tone_mapping_mode == ToneMapping::Relative && peak_luminance_nits_ < other_peak_luminance_nits) ? static_cast<float>(peak_luminance_nits_) / other_peak_luminance_nits : 1.0F;
      tone_adjustment *= boost_tone;

      if (std::fabs(tone_adjustment - 1.0F) > 1e-5) {
        post_filters.push_back("format=gbrpf32");

        if (tone_mapping_mode == ToneMapping::Auto) {
          // peak luma gets injected from within init_filters() during auto-mode
          post_filters.push_back("zscale=t=linear:npl=%d");
        } else {
          post_filters.push_back(string_sprintf("zscale=t=linear:npl=%d", peak_luminance_nits_));
        }

        post_filters.push_back(string_sprintf("tonemap=clip:param=%.5f", tone_adjustment));
        post_filters.push_back(string_sprintf("zscale=p=%s:t=%s", display_primaries.c_str(), display_trc.c_str()));
      } else {
        post_filters.push_back("format=rgb48");

        if (tone_mapping_mode == ToneMapping::Auto) {
          // peak luma gets injected from within init_filters() during auto-mode
          post_filters.push_back(string_sprintf("zscale=p=%s:t=%s:npl=%%d", display_primaries.c_str(), display_trc.c_str()));
        } else {
          post_filters.push_back(string_sprintf("zscale=p=%s:t=%s:npl=%d", display_primaries.c_str(), display_trc.c_str(), peak_luminance_nits_));
        }
      }
    } else {
      log_warning(string_sprintf("Cannot add tone mapping filters: %s", string_join(warnings, ", ").c_str()));
    }
  }

  if (!custom_post_filters.empty()) {
    post_filters.push_back(custom_post_filters);
  }

  pre_filter_description_ = string_join(pre_filters, ",");
  post_filter_description_ = string_join(post_filters, ",");

  init();
}

VideoFilterer::~VideoFilterer() {
  free();
}

void VideoFilterer::init() {
  filter_graph_ = avfilter_graph_alloc();

  ffmpeg::check(init_filters());
}

void VideoFilterer::free() {
  avfilter_graph_free(&filter_graph_);
}

void VideoFilterer::reinit() {
  free();
  init();
}

int VideoFilterer::init_filters() {
  AVFilterInOut* outputs = avfilter_inout_alloc();
  AVFilterInOut* inputs = avfilter_inout_alloc();

  int ret = 0;

  if ((outputs == nullptr) || (inputs == nullptr) || (filter_graph_ == nullptr)) {
    ret = AVERROR(ENOMEM);
  } else {
    const int sample_aspect_ratio_den = FFMAX(sample_aspect_ratio_.den, 1);
    const std::string args =
#if (LIBAVFILTER_VERSION_INT < AV_VERSION_INT(10, 1, 100))
        string_sprintf("video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", width_, height_, pixel_format_, time_base_.num, time_base_.den, sample_aspect_ratio_.num, sample_aspect_ratio_den);
#else
        string_sprintf("video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:colorspace=%d:range=%d", width_, height_, pixel_format_, time_base_.num, time_base_.den, sample_aspect_ratio_.num, sample_aspect_ratio_den, color_space_,
                       color_range_);
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

    const std::string base_filters = compose_filters(pre_filter_description_, post_filter_description_, crop_rect_, crop_enabled_);
    const std::string filters = (tone_mapping_mode_ == ToneMapping::Auto && dynamic_range_ != DynamicRange::Standard) ? string_sprintf(base_filters, peak_luminance_nits_) : base_filters;

    if ((ret = avfilter_graph_parse_ptr(filter_graph_, filters.c_str(), &inputs, &outputs, nullptr)) >= 0) {
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
    if (decoded_frame->sample_aspect_ratio.num > 0 && decoded_frame->sample_aspect_ratio.den > 0) {
      AVRational new_sar = decoded_frame->sample_aspect_ratio;
      av_reduce(&new_sar.num, &new_sar.den, new_sar.num, new_sar.den, MAX_AVRATIONAL_REDUCE);

      if (av_cmp_q(new_sar, sample_aspect_ratio_) != 0) {
        sample_aspect_ratio_ = new_sar;
        must_reinit = true;
      }
    }

    if (dynamic_range_ != DynamicRange::Standard) {
      unsigned max_cll = get_content_light_level_or_zero(decoded_frame);

      if (max_cll != UNSET_PEAK_LUMINANCE) {
        if (tone_mapping_mode_ == ToneMapping::FullRange || tone_mapping_mode_ == ToneMapping::Relative) {
          if (peak_luminance_nits_ != max_cll) {
            log_warning(string_sprintf("MaxCLL metadata (%d) differs from the expected HDR peak luminance (%d).", max_cll, peak_luminance_nits_));
          }
        } else if (tone_mapping_mode_ == ToneMapping::Auto && (peak_luminance_nits_ != max_cll)) {
          log_info(string_sprintf("HDR color space conversion adjusted to %d nits based on MaxCLL metadata.", max_cll).c_str());

          must_reinit = true;
        }

        peak_luminance_nits_ = max_cll;
      }
    }

    if (must_reinit) {
      mark_filter_changed();
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
  ffmpeg::frame_duration(filtered_frame) = av_rescale_q(ffmpeg::frame_duration(filtered_frame), time_base_, AV_R_MICROSECONDS);

  return true;
}

std::string VideoFilterer::filter_description() const {
  return compose_filters(pre_filter_description_, post_filter_description_, crop_rect_, crop_enabled_);
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

void VideoFilterer::mark_filter_changed() {
  filter_changed_.store(true, std::memory_order_release);
}

bool VideoFilterer::set_crop_rect(const CropRect* rect) {
  const auto reset_crop = [&]() {
    crop_enabled_ = false;
    crop_rect_ = CropRect{};
  };
  const auto rect_equals = [](const CropRect& a, const CropRect& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; };

  if (rect == nullptr) {
    const bool changed = crop_enabled_;
    reset_crop();
    if (changed) {
      mark_filter_changed();
    }
    return changed;
  }

  if (rect->w <= 0 || rect->h <= 0) {
    const bool changed = crop_enabled_;
    reset_crop();
    if (changed) {
      mark_filter_changed();
    }
    return changed;
  }

  const bool changed = !crop_enabled_ || !rect_equals(crop_rect_, *rect);
  crop_rect_ = *rect;
  crop_enabled_ = true;
  if (changed) {
    mark_filter_changed();
  }
  return changed;
}

bool VideoFilterer::consume_filter_change() {
  return filter_changed_.exchange(false, std::memory_order_acq_rel);
}