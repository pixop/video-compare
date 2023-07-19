#include "video_filterer.h"
#include <iostream>
#include <string>
#include "ffmpeg.h"
#include "string_utils.h"

VideoFilterer::VideoFilterer(const Demuxer* demuxer, const VideoDecoder* video_decoder, const std::string& custom_video_filters) : demuxer_(demuxer), video_decoder_(video_decoder) {
  std::vector<std::string> filters;

  if (!custom_video_filters.empty()) {
    filters.push_back(custom_video_filters);
  } else {
    if (demuxer->rotation() == 90) {
      filters.push_back("transpose=clock");
    } else if (demuxer->rotation() == 270) {
      filters.push_back("transpose=cclock");
    } else if (demuxer->rotation() == 180) {
      filters.push_back("hflip");
      filters.push_back("vflip");
    } else {
      filters.push_back("copy");
    }
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
    // buffer video source: the decoded frames go here
    const AVFilter* buffersrc = avfilter_get_by_name("buffer");
    const std::string args =
        string_sprintf("video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt, time_base.num, time_base.den, dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

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
  return av_buffersrc_add_frame_flags(buffersrc_ctx_, decoded_frame, AV_BUFFERSRC_FLAG_KEEP_REF) >= 0;
}

bool VideoFilterer::receive(AVFrame* filtered_frame) {
  auto ret = av_buffersink_get_frame_flags(buffersink_ctx_, filtered_frame, 0);

  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
    return false;
  }
  ffmpeg::check(ret);

  // update PTS
  const AVRational microseconds = {1, AV_TIME_BASE};
  filtered_frame->pts = av_rescale_q(filtered_frame->pts != AV_NOPTS_VALUE ? filtered_frame->pts : filtered_frame->best_effort_timestamp, av_buffersink_get_time_base(buffersink_ctx_), microseconds);

  return true;
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
