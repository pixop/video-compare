#include "video_filterer.h"
#include "ffmpeg.h"
#include <string>
#include <iostream>

VideoFilterer::VideoFilterer(const Demuxer *demuxer, const VideoDecoder *video_decoder) :
    video_decoder_(video_decoder),
    swap_dimensions_((demuxer->rotation() == 90) || (demuxer->rotation() == 270)) {
    char common_filters[256] = ""; // e.g. "yadif," (see FFmpeg documentation, please do not add any scaling and/or pixel format conversion filters for now)
    const char *rotation_filters;

    if (demuxer->rotation() == 90) {
        rotation_filters = "transpose=clock";
    } else if (demuxer->rotation() == 270) {
        rotation_filters = "transpose=cclock";
    } else if (demuxer->rotation() == 180) {
        rotation_filters = "hflip,vflip";
    } else {
        rotation_filters = "copy";
    }

    ffmpeg::check(init_filters(video_decoder->codec_context(), demuxer->time_base(), strcat(common_filters, rotation_filters)));
}

VideoFilterer::~VideoFilterer() {
    avfilter_graph_free(&filter_graph_);
}

int VideoFilterer::init_filters(const AVCodecContext *dec_ctx, const AVRational time_base, const char *filter_description)
{
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();

    filter_graph_ = avfilter_graph_alloc();

    if (!outputs || !inputs || !filter_graph_) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
            time_base.num, time_base.den,
            dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&buffersrc_ctx_, buffersrc, "in",
                                       args, nullptr, filter_graph_);
    if (ret < 0) {
        throw ffmpeg::Error{"Cannot create buffer source"};
    }

    /* buffer video sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&buffersink_ctx_, buffersink, "out",
                                       nullptr, nullptr, filter_graph_);
    if (ret < 0) {
        throw ffmpeg::Error{"Cannot create buffer sink"};
    }

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx_;
    outputs->pad_idx    = 0;
    outputs->next       = nullptr;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx_;
    inputs->pad_idx    = 0;
    inputs->next       = nullptr;

    if ((ret = avfilter_graph_parse_ptr(filter_graph_, filter_description, &inputs, &outputs, nullptr)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filter_graph_, nullptr)) < 0)
        goto end;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

bool VideoFilterer::send(AVFrame* decoded_frame) {
    return av_buffersrc_add_frame_flags(buffersrc_ctx_, decoded_frame, AV_BUFFERSRC_FLAG_KEEP_REF) >= 0;
}

bool VideoFilterer::receive(AVFrame* filtered_frame) {
    auto ret = av_buffersink_get_frame(buffersink_ctx_, filtered_frame);

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return false;
    } else {
        ffmpeg::check(ret);
        return true;
    }
}

size_t VideoFilterer::src_width() const {
    return video_decoder_->width();
}

size_t VideoFilterer::src_height() const {
    return video_decoder_->height();
}

size_t VideoFilterer::dest_width() const {
    return swap_dimensions_ ? src_height() : src_width();
}

size_t VideoFilterer::dest_height() const {
    return swap_dimensions_ ? src_width() : src_height();
}