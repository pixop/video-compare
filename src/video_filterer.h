#pragma once
#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include "core_types.h"
#include "demuxer.h"
#include "side_aware.h"
#include "video_decoder.h"
#include "video_filter_context.h"
#include "video_filter_state.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

namespace video_filter_state {

// Output of the pre-filter chain on an already-configured linear graph:
// buffersrc -> [pre-filters] -> [crop?] -> [post-filters] -> buffersink.
// Walks only the pre-filter links, so crop presence and post-filter geometry
// cannot change the result.
inline std::pair<int, int> crop_space_from_configured_chain(AVFilterContext* buffersrc, AVFilterContext* buffersink, const int pre_filter_count, const int fallback_width, const int fallback_height) {
  int width = std::max(1, fallback_width);
  int height = std::max(1, fallback_height);
  if (buffersrc == nullptr || buffersrc->nb_outputs == 0 || buffersrc->outputs[0] == nullptr) {
    return {width, height};
  }

  AVFilterLink* link = buffersrc->outputs[0];
  width = std::max(1, link->w);
  height = std::max(1, link->h);

  for (int i = 0; i < pre_filter_count; ++i) {
    if (link->dst == nullptr || link->dst == buffersink || link->dst->nb_outputs == 0 || link->dst->outputs[0] == nullptr) {
      break;
    }
    link = link->dst->outputs[0];
    width = std::max(1, link->w);
    height = std::max(1, link->h);
  }

  return {width, height};
}

}  // namespace video_filter_state

class VideoFilterer : public SideAware {
 public:
  VideoFilterer(const Side& side,
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
                const bool disable_auto_filters);
  ~VideoFilterer();

  void init();
  void free();
  void reinit();

  void close_src();

  bool send(AVFrame* decoded_frame);
  bool receive(AVFrame* filtered_frame);

  std::string filter_description() const;
  std::string resolved_filter_description() const;

  size_t src_width() const;
  size_t src_height() const;
  AVPixelFormat src_pixel_format() const;
  size_t dest_width() const;
  size_t dest_height() const;
  AVPixelFormat dest_pixel_format() const;
  // Pre-crop insertion size (after pre-filters, before crop/post-filters).
  // Independent of whether a crop is currently enabled.
  std::pair<int, int> crop_space_dimensions() const;
  bool interactive_crop_supported() const;

  bool set_crop_rect(const CropRect* rect);
  bool set_crop_state(const CropState& state);
  CropState crop_state() const;
  bool consume_filter_change();

 private:
  int init_filters();
  void capture_crop_space_dimensions();
  void capture_interactive_crop_support();

  void mark_filter_changed();

  const Demuxer* demuxer_;
  const VideoDecoder* video_decoder_;
  const ToneMapping tone_mapping_mode_;

  std::string pre_filter_description_;
  std::string post_filter_description_;

  int width_;
  int height_;
  int crop_space_width_;
  int crop_space_height_;
  bool interactive_crop_supported_{false};
  AVPixelFormat pixel_format_;
  AVColorSpace color_space_;
  AVColorRange color_range_;
  AVRational sample_aspect_ratio_;
  AVRational time_base_;

  AVFilterContext* buffersrc_ctx_;
  AVFilterContext* buffersink_ctx_;
  AVFilterGraph* filter_graph_;

  DynamicRange dynamic_range_;
  unsigned peak_luminance_nits_;

  CropState crop_{};
  CropState pending_crop_{};
  mutable std::mutex pending_crop_mutex_;

  std::atomic_bool filter_changed_{false};
  std::atomic<int> filter_generation_{0};
};
