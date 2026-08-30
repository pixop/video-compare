#pragma once

#include <cstdint>
#include <string>
extern "C" {
#include <libavutil/rational.h>
}

struct AVFrame;

namespace FrameMetadata {

std::string require_key(const AVFrame* frame);
void set_key(AVFrame* frame, const std::string& key);

int get_filter_generation(const AVFrame* frame);
void set_filter_generation(AVFrame* frame, int generation);

std::string get_resolved_filters(const AVFrame* frame);
void set_resolved_filters(AVFrame* frame, const std::string& filters);

int get_original_width(const AVFrame* frame, int fallback);
int get_original_height(const AVFrame* frame, int fallback);
void set_original_dimensions(AVFrame* frame, int width, int height);

// Effective display aspect ratio from post-filter (pre-canvas) geometry.
// Uses width/height * SAR via av_mul_q; invalid/unspecified SAR is treated as 1:1.
// Returns false when width/height are not positive or dar is null.
inline bool display_aspect_ratio(int width, int height, AVRational sar, AVRational* dar) {
  if (dar == nullptr || width <= 0 || height <= 0) {
    return false;
  }

  const AVRational sanitized_sar = (sar.num <= 0 || sar.den <= 0) ? AVRational{1, 1} : sar;
  *dar = av_mul_q(AVRational{width, height}, sanitized_sar);
  return dar->num > 0 && dar->den > 0;
}
// Same, from a converted frame's original_width/height metadata + sample_aspect_ratio.
// Never uses converted frame->width/height (shared canvas). Missing original dims => false.
bool try_display_aspect_ratio(const AVFrame* frame, AVRational* dar);

std::string make_frame_key(int64_t pts, int filter_generation);
std::string make_frame_key(const AVFrame* frame);

}  // namespace FrameMetadata
