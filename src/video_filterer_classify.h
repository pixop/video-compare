#pragma once

#include <cstring>

extern "C" {
#include <libavfilter/avfilter.h>
}

// Implementation detail for VideoFilterer and its unit tests.
// Not part of the public VideoCompare / crop-state API.
namespace video_filterer_detail {

inline bool post_crop_filter_is_supported(AVFilterContext* ctx) {
  if (ctx == nullptr || ctx->filter == nullptr || ctx->filter->name == nullptr) {
    return false;
  }
  if (ctx->nb_inputs != 1 || ctx->nb_outputs != 1 || ctx->inputs[0] == nullptr || ctx->outputs[0] == nullptr) {
    return false;
  }

  const char* name = ctx->filter->name;
  if (std::strcmp(name, "crop") == 0 || std::strcmp(name, "pad") == 0 || std::strcmp(name, "transpose") == 0 || std::strcmp(name, "rotate") == 0 || std::strcmp(name, "hflip") == 0 || std::strcmp(name, "vflip") == 0 ||
      std::strcmp(name, "perspective") == 0) {
    return false;
  }
  if (std::strcmp(name, "scale") == 0 || std::strcmp(name, "zscale") == 0 || std::strcmp(name, "copy") == 0 || std::strcmp(name, "null") == 0 || std::strcmp(name, "format") == 0 || std::strcmp(name, "colorspace") == 0 ||
      std::strcmp(name, "tonemap") == 0 || std::strcmp(name, "setparams") == 0 || std::strcmp(name, "setsar") == 0 || std::strcmp(name, "setdar") == 0) {
    return true;
  }
  return ctx->inputs[0]->w == ctx->outputs[0]->w && ctx->inputs[0]->h == ctx->outputs[0]->h;
}

// Axis-aligned resize and coordinate-preserving filters are supported.
// Known spatial remaps and non-linear topology are unsupported. Unknown
// same-size filters are permitted; an unknown same-size geometric warp
// can still evade this heuristic.
inline bool interactive_crop_supported_from_configured_chain(AVFilterContext* buffersrc, AVFilterContext* buffersink, const int pre_filter_count, const bool app_crop_enabled) {
  if (buffersrc == nullptr || buffersink == nullptr || buffersrc->nb_outputs == 0 || buffersrc->outputs[0] == nullptr) {
    return false;
  }

  AVFilterLink* link = buffersrc->outputs[0];
  for (int i = 0; i < pre_filter_count; ++i) {
    AVFilterContext* ctx = link->dst;
    if (ctx == nullptr || ctx == buffersink || ctx->nb_outputs != 1 || ctx->outputs[0] == nullptr) {
      return false;
    }
    link = ctx->outputs[0];
  }

  AVFilterContext* ctx = link->dst;
  if (app_crop_enabled) {
    if (ctx == nullptr || ctx == buffersink || ctx->filter == nullptr || ctx->filter->name == nullptr || std::strcmp(ctx->filter->name, "crop") != 0) {
      return false;
    }
    if (ctx->nb_outputs != 1 || ctx->outputs[0] == nullptr) {
      return false;
    }
    ctx = ctx->outputs[0]->dst;
  }

  while (ctx != nullptr && ctx != buffersink) {
    if (!post_crop_filter_is_supported(ctx)) {
      return false;
    }
    ctx = ctx->outputs[0]->dst;
  }
  return ctx == buffersink;
}

}  // namespace video_filterer_detail
