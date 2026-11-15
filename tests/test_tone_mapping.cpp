#include "tone_mapping.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

static int failures = 0;

static void expect_true(const char* label, const bool condition) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s\n", label);
    failures++;
    return;
  }
  std::printf("PASS %s\n", label);
}

static void expect_eq_str(const char* label, const std::string& got, const std::string& expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: expected '%s', got '%s'\n", label, expected.c_str(), got.c_str());
    failures++;
    return;
  }
  std::printf("PASS %s\n", label);
}

static void expect_gt(const char* label, const double got, const double min_expected) {
  if (!(got > min_expected)) {
    std::fprintf(stderr, "FAIL %s: expected > %f, got %f\n", label, min_expected, got);
    failures++;
    return;
  }
  std::printf("PASS %s (%f > %f)\n", label, got, min_expected);
}

static void expect_throws(const char* label, const std::function<void()>& fn) {
  try {
    fn();
    std::fprintf(stderr, "FAIL %s: expected exception\n", label);
    failures++;
  } catch (const std::exception&) {
    std::printf("PASS %s\n", label);
  }
}

static std::string join_filters(const std::vector<std::string>& filters) {
  std::string joined;
  for (size_t i = 0; i < filters.size(); ++i) {
    if (i > 0) {
      joined += ',';
    }
    joined += filters[i];
  }
  return joined;
}

static double measure_highlight_luma(const std::string& filter_suffix, const float highlight_linear) {
  AVFilterGraph* graph = avfilter_graph_alloc();
  AVFilterInOut* outputs = avfilter_inout_alloc();
  AVFilterInOut* inputs = avfilter_inout_alloc();
  if (graph == nullptr || outputs == nullptr || inputs == nullptr) {
    throw std::runtime_error("filter alloc failed");
  }

  const int width = 64;
  const int height = 64;
  const std::string args = "video_size=" + std::to_string(width) + "x" + std::to_string(height) + ":pix_fmt=" + std::to_string(static_cast<int>(AV_PIX_FMT_GBRPF32LE)) + ":time_base=1/25:pixel_aspect=1/1";

  AVFilterContext* src = nullptr;
  AVFilterContext* sink = nullptr;
  int ret = avfilter_graph_create_filter(&src, avfilter_get_by_name("buffer"), "in", args.c_str(), nullptr, graph);
  if (ret >= 0) {
    ret = avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph);
  }
  if (ret < 0) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&graph);
    throw std::runtime_error("filter create failed");
  }

  outputs->name = av_strdup("in");
  outputs->filter_ctx = src;
  outputs->pad_idx = 0;
  outputs->next = nullptr;

  inputs->name = av_strdup("out");
  inputs->filter_ctx = sink;
  inputs->pad_idx = 0;
  inputs->next = nullptr;

  const std::string filters = filter_suffix;
  ret = avfilter_graph_parse_ptr(graph, filters.c_str(), &inputs, &outputs, nullptr);
  if (ret >= 0) {
    ret = avfilter_graph_config(graph, nullptr);
  }

  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);

  if (ret < 0) {
    avfilter_graph_free(&graph);
    throw std::runtime_error("filter graph failed");
  }

  AVFrame* in_frame = av_frame_alloc();
  in_frame->width = width;
  in_frame->height = height;
  in_frame->format = AV_PIX_FMT_GBRPF32LE;
  in_frame->color_primaries = AVCOL_PRI_BT709;
  in_frame->color_trc = AVCOL_TRC_LINEAR;
  in_frame->colorspace = AVCOL_SPC_RGB;
  in_frame->color_range = AVCOL_RANGE_JPEG;
  av_frame_get_buffer(in_frame, 32);

  const float shadow = 0.05F;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float level = (x < width / 2) ? shadow : highlight_linear;
      for (int c = 0; c < 3; ++c) {
        auto* plane = reinterpret_cast<float*>(in_frame->data[c] + y * in_frame->linesize[c]);
        plane[x] = level;
      }
    }
  }

  ret = av_buffersrc_add_frame_flags(src, in_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
  av_frame_free(&in_frame);
  if (ret < 0) {
    avfilter_graph_free(&graph);
    throw std::runtime_error("buffersrc failed");
  }

  AVFrame* out_frame = av_frame_alloc();
  ret = av_buffersink_get_frame_flags(sink, out_frame, 0);
  avfilter_graph_free(&graph);

  if (ret < 0) {
    av_frame_free(&out_frame);
    throw std::runtime_error("buffersink failed");
  }

  double highlight_max = 0.0;
  double shadow_max = 0.0;
  const int out_width = out_frame->width;
  const int out_height = out_frame->height;

  if (out_frame->format == AV_PIX_FMT_RGB48LE) {
    for (int y = 0; y < out_height; ++y) {
      const uint16_t* row = reinterpret_cast<const uint16_t*>(out_frame->data[0] + y * out_frame->linesize[0]);
      for (int x = 0; x < out_width; ++x) {
        const double sample = static_cast<double>(row[x * 3]) / 65535.0;
        if (x < out_width / 2) {
          shadow_max = std::max(shadow_max, sample);
        } else {
          highlight_max = std::max(highlight_max, sample);
        }
      }
    }
  } else if (out_frame->format == AV_PIX_FMT_GBRPF32LE) {
    for (int y = 0; y < out_height; ++y) {
      for (int x = 0; x < out_width; ++x) {
        double sample = 0.0;
        for (int c = 0; c < 3; ++c) {
          const float* row = reinterpret_cast<const float*>(out_frame->data[c] + y * out_frame->linesize[c]);
          sample = std::max(sample, static_cast<double>(row[x]));
        }
        if (x < out_width / 2) {
          shadow_max = std::max(shadow_max, sample);
        } else {
          highlight_max = std::max(highlight_max, sample);
        }
      }
    }
  }

  av_frame_free(&out_frame);
  return highlight_max - shadow_max;
}

static double max_rgb_sample(const AVFrame* frame) {
  double highlight_max = 0.0;
  if (frame->format == AV_PIX_FMT_RGB48LE) {
    for (int y = 0; y < frame->height; ++y) {
      const uint16_t* row = reinterpret_cast<const uint16_t*>(frame->data[0] + y * frame->linesize[0]);
      for (int x = 0; x < frame->width; ++x) {
        highlight_max = std::max(highlight_max, static_cast<double>(row[x * 3]) / 65535.0);
      }
    }
  } else if (frame->format == AV_PIX_FMT_GBRP16LE || frame->format == AV_PIX_FMT_GBRP16BE) {
    for (int y = 0; y < frame->height; ++y) {
      for (int x = 0; x < frame->width; ++x) {
        for (int c = 0; c < 3; ++c) {
          const uint16_t* row = reinterpret_cast<const uint16_t*>(frame->data[c] + y * frame->linesize[c]);
          highlight_max = std::max(highlight_max, static_cast<double>(row[x]) / 65535.0);
        }
      }
    }
  } else if (frame->format == AV_PIX_FMT_GBRPF32LE) {
    for (int y = 0; y < frame->height; ++y) {
      for (int x = 0; x < frame->width; ++x) {
        for (int c = 0; c < 3; ++c) {
          const float* row = reinterpret_cast<const float*>(frame->data[c] + y * frame->linesize[c]);
          highlight_max = std::max(highlight_max, static_cast<double>(row[x]));
        }
      }
    }
  }
  return highlight_max;
}

static double measure_pq_white_highlight(const std::string& filters) {
  AVFilterGraph* graph = avfilter_graph_alloc();
  AVFilterInOut* outputs = avfilter_inout_alloc();
  AVFilterInOut* inputs = avfilter_inout_alloc();
  if (graph == nullptr || outputs == nullptr || inputs == nullptr) {
    throw std::runtime_error("filter alloc failed");
  }

  const int width = 64;
  const int height = 64;
  const std::string args = "video_size=" + std::to_string(width) + "x" + std::to_string(height) + ":pix_fmt=" + std::to_string(static_cast<int>(AV_PIX_FMT_YUV420P)) + ":time_base=1/25:pixel_aspect=1/1";

  AVFilterContext* src = nullptr;
  AVFilterContext* sink = nullptr;
  int ret = avfilter_graph_create_filter(&src, avfilter_get_by_name("buffer"), "in", args.c_str(), nullptr, graph);
  if (ret >= 0) {
    ret = avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph);
  }
  if (ret < 0) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&graph);
    throw std::runtime_error("filter create failed");
  }

  outputs->name = av_strdup("in");
  outputs->filter_ctx = src;
  outputs->pad_idx = 0;
  outputs->next = nullptr;

  inputs->name = av_strdup("out");
  inputs->filter_ctx = sink;
  inputs->pad_idx = 0;
  inputs->next = nullptr;

  ret = avfilter_graph_parse_ptr(graph, filters.c_str(), &inputs, &outputs, nullptr);
  if (ret >= 0) {
    ret = avfilter_graph_config(graph, nullptr);
  }

  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  if (ret < 0) {
    avfilter_graph_free(&graph);
    throw std::runtime_error("filter graph failed");
  }

  AVFrame* in_frame = av_frame_alloc();
  in_frame->width = width;
  in_frame->height = height;
  in_frame->format = AV_PIX_FMT_YUV420P;
  in_frame->color_primaries = AVCOL_PRI_BT2020;
  in_frame->color_trc = AVCOL_TRC_SMPTE2084;
  in_frame->colorspace = AVCOL_SPC_BT2020_NCL;
  in_frame->color_range = AVCOL_RANGE_MPEG;
  av_frame_get_buffer(in_frame, 32);

  for (int y = 0; y < height; ++y) {
    std::memset(in_frame->data[0] + y * in_frame->linesize[0], 255, width);
  }
  for (int y = 0; y < height / 2; ++y) {
    std::memset(in_frame->data[1] + y * in_frame->linesize[1], 128, width / 2);
    std::memset(in_frame->data[2] + y * in_frame->linesize[2], 128, width / 2);
  }

  ret = av_buffersrc_add_frame_flags(src, in_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
  av_frame_free(&in_frame);
  if (ret < 0) {
    avfilter_graph_free(&graph);
    throw std::runtime_error("buffersrc failed");
  }

  AVFrame* out_frame = av_frame_alloc();
  ret = av_buffersink_get_frame_flags(sink, out_frame, 0);
  avfilter_graph_free(&graph);
  if (ret < 0) {
    av_frame_free(&out_frame);
    throw std::runtime_error("buffersink failed");
  }

  const double peak = max_rgb_sample(out_frame);
  av_frame_free(&out_frame);
  return peak;
}

static double measure_highlight_max(const std::string& filter_suffix, const float highlight_linear) {
  AVFilterGraph* graph = avfilter_graph_alloc();
  AVFilterInOut* outputs = avfilter_inout_alloc();
  AVFilterInOut* inputs = avfilter_inout_alloc();
  if (graph == nullptr || outputs == nullptr || inputs == nullptr) {
    throw std::runtime_error("filter alloc failed");
  }

  const int width = 64;
  const int height = 64;
  const std::string args = "video_size=" + std::to_string(width) + "x" + std::to_string(height) + ":pix_fmt=" + std::to_string(static_cast<int>(AV_PIX_FMT_GBRPF32LE)) + ":time_base=1/25:pixel_aspect=1/1";

  AVFilterContext* src = nullptr;
  AVFilterContext* sink = nullptr;
  int ret = avfilter_graph_create_filter(&src, avfilter_get_by_name("buffer"), "in", args.c_str(), nullptr, graph);
  if (ret >= 0) {
    ret = avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph);
  }
  if (ret < 0) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&graph);
    throw std::runtime_error("filter create failed");
  }

  outputs->name = av_strdup("in");
  outputs->filter_ctx = src;
  outputs->pad_idx = 0;
  outputs->next = nullptr;

  inputs->name = av_strdup("out");
  inputs->filter_ctx = sink;
  inputs->pad_idx = 0;
  inputs->next = nullptr;

  ret = avfilter_graph_parse_ptr(graph, filter_suffix.c_str(), &inputs, &outputs, nullptr);
  if (ret >= 0) {
    ret = avfilter_graph_config(graph, nullptr);
  }

  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);

  if (ret < 0) {
    avfilter_graph_free(&graph);
    throw std::runtime_error("filter graph failed");
  }

  AVFrame* in_frame = av_frame_alloc();
  in_frame->width = width;
  in_frame->height = height;
  in_frame->format = AV_PIX_FMT_GBRPF32LE;
  in_frame->color_primaries = AVCOL_PRI_BT709;
  in_frame->color_trc = AVCOL_TRC_LINEAR;
  in_frame->colorspace = AVCOL_SPC_RGB;
  in_frame->color_range = AVCOL_RANGE_JPEG;
  av_frame_get_buffer(in_frame, 32);

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float level = (x < width / 2) ? 0.05F : highlight_linear;
      for (int c = 0; c < 3; ++c) {
        auto* plane = reinterpret_cast<float*>(in_frame->data[c] + y * in_frame->linesize[c]);
        plane[x] = level;
      }
    }
  }

  ret = av_buffersrc_add_frame_flags(src, in_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
  av_frame_free(&in_frame);
  if (ret < 0) {
    avfilter_graph_free(&graph);
    throw std::runtime_error("buffersrc failed");
  }

  AVFrame* out_frame = av_frame_alloc();
  ret = av_buffersink_get_frame_flags(sink, out_frame, 0);
  avfilter_graph_free(&graph);
  if (ret < 0) {
    av_frame_free(&out_frame);
    throw std::runtime_error("buffersink failed");
  }

  double highlight_max = 0.0;
  if (out_frame->format == AV_PIX_FMT_RGB48LE) {
    for (int y = 0; y < out_frame->height; ++y) {
      const uint16_t* row = reinterpret_cast<const uint16_t*>(out_frame->data[0] + y * out_frame->linesize[0]);
      for (int x = out_frame->width / 2; x < out_frame->width; ++x) {
        highlight_max = std::max(highlight_max, static_cast<double>(row[x * 3]) / 65535.0);
      }
    }
  } else if (out_frame->format == AV_PIX_FMT_GBRPF32LE) {
    for (int y = 0; y < out_frame->height; ++y) {
      for (int x = out_frame->width / 2; x < out_frame->width; ++x) {
        for (int c = 0; c < 3; ++c) {
          const float* row = reinterpret_cast<const float*>(out_frame->data[c] + y * out_frame->linesize[c]);
          highlight_max = std::max(highlight_max, static_cast<double>(row[x]));
        }
      }
    }
  }
  av_frame_free(&out_frame);
  return highlight_max;
}

int main() {
  expect_eq_str("default algorithm name", tone_map_algorithm_name(parse_tone_map_algorithm("")), "clip");
  expect_eq_str("mobius parse", tone_map_algorithm_name(parse_tone_map_algorithm("mobius")), "mobius");

  expect_throws("invalid algorithm", [] { parse_tone_map_algorithm("linear"); });
  expect_throws("rel rejects mobius", [] { validate_tone_mapping_for_side("Left", ToneMapping::Relative, ToneMapAlgorithm::Mobius); });
  validate_tone_mapping_for_side("Left", ToneMapping::Off, ToneMapAlgorithm::Hable);

  expect_eq_str("normalized 1000 nits", format_normalized_tonemap_peak(1000, 1.0F), "10.000000");
  expect_eq_str("normalized MaxCLL 1230", format_normalized_tonemap_peak(1230, 1.0F), "12.300000");
  expect_eq_str("normalized with boost", format_normalized_tonemap_peak(500, 2.0F), "10.000000");
  expect_eq_str("SDR source peak", format_normalized_tonemap_peak(100, 1.0F), "1.000000");

  const ToneMapFilterPlan clip_default = build_hdr_tone_map_filters(ToneMapping::FullRange, ToneMapAlgorithm::Clip, 1.0F, 500, 500, true, true);
  expect_eq_str("clip boost=1 graph", join_filters(clip_default.post_filters), "format=rgb48,zscale=p=bt709:t=iec61966-2-1:npl=500");
  expect_true("clip boost=1 no peak inject", !clip_default.inject_peak_nits && !clip_default.inject_auto_tonemap_peak);

  const ToneMapFilterPlan clip_boost = build_hdr_tone_map_filters(ToneMapping::FullRange, ToneMapAlgorithm::Clip, 1.5F, 500, 500, true, true);
  expect_eq_str("clip boost graph", join_filters(clip_boost.post_filters),
                "format=gbrpf32,zscale=t=linear:npl=500,tonemap=clip:param=1.50000,zscale=p=bt709:t=iec61966-2-1");

  const ToneMapFilterPlan clip_rel = build_hdr_tone_map_filters(ToneMapping::Relative, ToneMapAlgorithm::Clip, 1.0F, 400, 800, true, true);
  expect_eq_str("clip rel ratio", join_filters(clip_rel.post_filters),
                "format=gbrpf32,zscale=t=linear:npl=400,tonemap=clip:param=0.50000,zscale=p=bt709:t=iec61966-2-1");

  const ToneMapFilterPlan mobius = build_hdr_tone_map_filters(ToneMapping::FullRange, ToneMapAlgorithm::Mobius, 1.0F, 1000, 500, true, true);
  expect_eq_str("mobius base graph", join_filters(mobius.post_filters),
                "format=gbrpf32,zscale=t=linear:npl=100,tonemap=mobius:peak=10.000000,zscale=p=bt709:t=iec61966-2-1");

  const ToneMapFilterPlan mobius_boost = build_hdr_tone_map_filters(ToneMapping::FullRange, ToneMapAlgorithm::Mobius, 2.0F, 1000, 500, true, true);
  expect_eq_str("mobius boost uses exposure and scaled peak",
                join_filters(mobius_boost.post_filters),
                "format=gbrpf32,zscale=t=linear:npl=100,exposure=1.000000,tonemap=mobius:peak=20.000000,zscale=p=bt709:t=iec61966-2-1");

  expect_throws("nonlinear boost above exposure range", [] {
    build_hdr_tone_map_filters(ToneMapping::FullRange, ToneMapAlgorithm::Hable, 9.0F, 1000, 500, true, true);
  });

  const ToneMapFilterPlan auto_clip = build_hdr_tone_map_filters(ToneMapping::Auto, ToneMapAlgorithm::Clip, 1.0F, 500, 500, true, true);
  expect_eq_str("auto clip inject", join_filters(auto_clip.post_filters), "format=rgb48,zscale=p=bt709:t=iec61966-2-1:npl=%d");
  expect_true("auto clip peak placeholder only", auto_clip.inject_peak_nits && !auto_clip.inject_auto_tonemap_peak);

  const ToneMapFilterPlan auto_mobius = build_hdr_tone_map_filters(ToneMapping::Auto, ToneMapAlgorithm::Reinhard, 1.0F, 500, 500, true, true);
  expect_eq_str("auto reinhard uses token placeholder", join_filters(auto_mobius.post_filters),
                "format=gbrpf32,zscale=t=linear:npl=100,tonemap=reinhard:peak=@@TONEMAP_NORM_PEAK@@,zscale=p=bt709:t=iec61966-2-1");
  expect_true("auto nonlinear tonemap token only", !auto_mobius.inject_peak_nits && auto_mobius.inject_auto_tonemap_peak);

  const std::string auto_template = join_filters(auto_mobius.post_filters);
  expect_eq_str("resolve MaxCLL 500", resolve_tone_map_filter_description(auto_template, 500, 1.0F, false, true),
                "format=gbrpf32,zscale=t=linear:npl=100,tonemap=reinhard:peak=5.000000,zscale=p=bt709:t=iec61966-2-1");
  expect_eq_str("resolve MaxCLL 1230", resolve_tone_map_filter_description(auto_template, 1230, 1.0F, false, true),
                "format=gbrpf32,zscale=t=linear:npl=100,tonemap=reinhard:peak=12.300000,zscale=p=bt709:t=iec61966-2-1");
  expect_eq_str("resolve auto clip nits only", resolve_tone_map_filter_description(join_filters(auto_clip.post_filters), 1230, 1.0F, true, false),
                "format=rgb48,zscale=p=bt709:t=iec61966-2-1:npl=1230");

  const bool have_zscale = avfilter_get_by_name("zscale") != nullptr;
  const bool have_tonemap = avfilter_get_by_name("tonemap") != nullptr;
  const bool have_exposure = avfilter_get_by_name("exposure") != nullptr;

  if (have_zscale && have_tonemap && have_exposure) {
    const std::string correct = join_filters(mobius.post_filters);
    try {
      const double correct_sep = measure_highlight_luma(correct, 5.0F);
      expect_gt("correct tonemap keeps highlight separation", correct_sep, 0.03);

      const std::string pq_correct = "format=yuv420p,setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc,"
                                    "zscale=t=linear:npl=100,format=gbrpf32,tonemap=mobius:peak=10,zscale=p=bt709:t=iec61966-2-1";
      const std::string pq_wrong = "format=yuv420p,setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc,"
                                   "zscale=t=linear:npl=1000,format=gbrpf32,tonemap=mobius:peak=1000,zscale=p=bt709:t=iec61966-2-1";
      const double pq_correct_h = measure_pq_white_highlight(pq_correct);
      const double pq_wrong_h = measure_pq_white_highlight(pq_wrong);
      expect_gt("PQ path normalized peak brighter than nits peak bug", pq_correct_h, pq_wrong_h);
      expect_gt("PQ correct path produces bright output", pq_correct_h, 0.5);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "FAIL pixel tonemap tests: %s\n", e.what());
      failures++;
    }

    try {
      const ToneMapFilterPlan boosted = build_hdr_tone_map_filters(ToneMapping::FullRange, ToneMapAlgorithm::Mobius, 2.0F, 1000, 500, true, true);
      const std::string boosted_graph = join_filters(boosted.post_filters);
      expect_true("boost graph contains exposure and scaled peak", boosted_graph.find("exposure=") != std::string::npos && boosted_graph.find("peak=20.000000") != std::string::npos);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "FAIL boost pixel tests: %s\n", e.what());
      failures++;
    }

    expect_true("resolved nonlinear graph configures", [&mobius]() {
      try {
        measure_highlight_luma(resolve_tone_map_filter_description(join_filters(mobius.post_filters), 1000, 1.0F, false, false), 5.0F);
        return true;
      } catch (...) {
        return false;
      }
    }());
    expect_true("clip rel filter graph configures", [&clip_rel]() {
      AVFilterGraph* graph = avfilter_graph_alloc();
      if (graph == nullptr) {
        return false;
      }
      const std::string chain = join_filters(clip_rel.post_filters);
      AVFilterInOut* outputs = avfilter_inout_alloc();
      AVFilterInOut* inputs = avfilter_inout_alloc();
      if (outputs == nullptr || inputs == nullptr) {
        avfilter_graph_free(&graph);
        return false;
      }
      const std::string args = "video_size=64x64:pix_fmt=" + std::to_string(static_cast<int>(AV_PIX_FMT_YUV420P)) + ":time_base=1/25:pixel_aspect=1/1";
      AVFilterContext* src = nullptr;
      AVFilterContext* sink = nullptr;
      int ret = avfilter_graph_create_filter(&src, avfilter_get_by_name("buffer"), "in", args.c_str(), nullptr, graph);
      if (ret >= 0) {
        ret = avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph);
      }
      if (ret < 0) {
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        avfilter_graph_free(&graph);
        return false;
      }
      outputs->name = av_strdup("in");
      outputs->filter_ctx = src;
      outputs->pad_idx = 0;
      outputs->next = nullptr;
      inputs->name = av_strdup("out");
      inputs->filter_ctx = sink;
      inputs->pad_idx = 0;
      inputs->next = nullptr;
      ret = avfilter_graph_parse_ptr(graph, chain.c_str(), &inputs, &outputs, nullptr);
      if (ret >= 0) {
        ret = avfilter_graph_config(graph, nullptr);
      }
      avfilter_inout_free(&inputs);
      avfilter_inout_free(&outputs);
      avfilter_graph_free(&graph);
      return ret >= 0;
    }());
  } else {
    std::printf("SKIP live filter graph (zscale=%d tonemap=%d exposure=%d)\n", static_cast<int>(have_zscale), static_cast<int>(have_tonemap), static_cast<int>(have_exposure));
  }

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }

  std::printf("All tone mapping tests passed\n");
  return EXIT_SUCCESS;
}