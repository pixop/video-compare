#pragma once
#include "demuxer.h"
#include "video_decoder.h"
extern "C" {
    #include "libavcodec/avcodec.h"
    #include <libavfilter/buffersink.h>
    #include <libavfilter/buffersrc.h>
}

class VideoFilterer {
public:
    VideoFilterer(const Demuxer *demuxer, const VideoDecoder *video_decoder);
    ~VideoFilterer();

    bool send(AVFrame* decoded_frame);
    bool receive(AVFrame* filtered_frame);

    size_t src_width() const;
    size_t src_height() const;
    size_t dest_width() const;
    size_t dest_height() const;

private:
    int init_filters(const AVCodecContext *dec_ctx, const AVRational time_base, const char *filter_description);

private:
    const VideoDecoder *video_decoder_;

    bool swap_dimensions_;

    AVFilterContext *buffersink_ctx_;
    AVFilterContext *buffersrc_ctx_;
    AVFilterGraph *filter_graph_;
};
