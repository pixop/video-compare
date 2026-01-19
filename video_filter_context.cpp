#include "video_filter_context.h"
#include <algorithm>
extern "C" {
#include <libavutil/rational.h>
}

void VideoFilterContext::add(const Side& side, const Demuxer* demuxer, const VideoDecoder* decoder, const std::string& custom_color_trc) {
  videos_[side] = VideoInfo{demuxer, decoder, custom_color_trc};
}

bool VideoFilterContext::is_interlaced(const VideoDecoder* decoder) {
  return decoder->codec_context()->field_order != AV_FIELD_PROGRESSIVE && decoder->codec_context()->field_order != AV_FIELD_UNKNOWN;
}

double VideoFilterContext::get_max_frame_rate_excluding(const Side& side) const {
  double max_frame_rate = 0.0;

  for (const auto& pair : videos_) {
    const Side& other_side = pair.first;
    if (other_side != side) {
      const VideoInfo& info = pair.second;
      double frame_rate = av_q2d(info.demuxer->guess_frame_rate());

      // Account for interlacing
      if (is_interlaced(info.decoder)) {
        frame_rate *= 2.0;
      }

      max_frame_rate = std::max(max_frame_rate, frame_rate);
    }
  }

  return max_frame_rate;
}

unsigned VideoFilterContext::get_max_peak_luminance_excluding(const Side& side) const {
  unsigned max_peak_luminance = 0;

  for (const auto& pair : videos_) {
    const Side& other_side = pair.first;
    if (other_side != side) {
      const VideoInfo& info = pair.second;
      const DynamicRange dynamic_range = info.decoder->infer_dynamic_range(info.custom_color_trc);
      const unsigned peak_luminance = info.decoder->safe_peak_luminance_nits(dynamic_range);

      max_peak_luminance = std::max(max_peak_luminance, peak_luminance);
    }
  }

  return max_peak_luminance;
}
