#pragma once
#include <map>
#include <string>
#include "core_types.h"
#include "demuxer.h"
#include "video_decoder.h"

// VideoFilterContext encapsulates a set of videos (left + right videos) and provides
// methods to determine reference videos for auto-filter determination.
class VideoFilterContext {
 public:
  struct VideoInfo {
    const Demuxer* demuxer;
    const VideoDecoder* decoder;
    std::string custom_color_trc;
  };

  void add(const Side& side, const Demuxer* demuxer, const VideoDecoder* decoder, const std::string& custom_color_trc);

  // Get the maximum frame rate among all videos except the given side
  // (used for frame rate harmonization)
  double get_max_frame_rate_excluding(const Side& side) const;

  // Get the maximum peak luminance (in nits) among all videos except the given side
  // (used for relative tone mapping)
  unsigned get_max_peak_luminance_excluding(const Side& side) const;

 private:
  static bool is_interlaced(const VideoDecoder* decoder);

  std::map<Side, VideoInfo> videos_;
};
