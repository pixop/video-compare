#pragma once
#include "demuxer.h"
#include "video_decoder.h"
extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include "libavcodec/avcodec.h"
}

class VideoFilterer {
 public:
  VideoFilterer(const Demuxer* demuxer, const VideoDecoder* video_decoder);
  ~VideoFilterer();

  bool send(AVFrame* decoded_frame);
  bool receive(AVFrame* filtered_frame);

  size_t src_width() const;
  size_t src_height() const;
  AVPixelFormat src_pixel_format() const;
  size_t dest_width() const;
  size_t dest_height() const;
  AVPixelFormat dest_pixel_format() const;

 private:
  int init_filters(const AVCodecContext* dec_ctx, AVRational time_base, const std::string& filter_description);

  AVFilterContext* buffersrc_ctx_;
  AVFilterContext* buffersink_ctx_;
  AVFilterGraph* filter_graph_;
};
