#define SDL_MAIN_HANDLED
#include "scope_color.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
}

static int failures = 0;

static void expect(const bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s\n", message);
    failures++;
    return;
  }
  std::printf("PASS %s\n", message);
}

static void expect_eq(const char* label, const std::string& got, const std::string& expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: expected '%s', got '%s'\n", label, expected.c_str(), got.c_str());
    failures++;
    return;
  }
  std::printf("PASS %s\n", label);
}

static void expect_mode(const char* label, const ScopeColorMode got, const ScopeColorMode expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s\n", label);
    failures++;
    return;
  }
  std::printf("PASS %s\n", label);
}

static void expect_modes(const char* label, const ScopeColorModes& got, const ScopeColorMode histogram, const ScopeColorMode vectorscope, const ScopeColorMode waveform) {
  if (got.histogram != histogram || got.vectorscope != vectorscope || got.waveform != waveform) {
    std::fprintf(stderr, "FAIL %s\n", label);
    failures++;
    return;
  }
  std::printf("PASS %s\n", label);
}

static void expect_error(const char* label, const std::string& spec, const std::string& expected_message) {
  try {
    parse_scope_color_modes(spec);
    std::fprintf(stderr, "FAIL %s: expected error '%s'\n", label, expected_message.c_str());
    failures++;
  } catch (const std::logic_error& error) {
    if (error.what() != expected_message) {
      std::fprintf(stderr, "FAIL %s: expected '%s', got '%s'\n", label, expected_message.c_str(), error.what());
      failures++;
      return;
    }
    std::printf("PASS %s\n", label);
  }
}

struct Rgb {
  int r;
  int g;
  int b;
};

struct PackedFrame {
  int width{0};
  int height{0};
  std::vector<uint8_t> rgb;
};

struct FilterGraph {
  AVFilterGraph* graph{nullptr};
  AVFilterContext* src{nullptr};
  AVFilterContext* sink{nullptr};

  FilterGraph() = default;
  FilterGraph(const FilterGraph&) = delete;
  FilterGraph& operator=(const FilterGraph&) = delete;
  FilterGraph(FilterGraph&& other) noexcept : graph(other.graph), src(other.src), sink(other.sink) {
    other.graph = nullptr;
    other.src = nullptr;
    other.sink = nullptr;
  }
  FilterGraph& operator=(FilterGraph&&) = delete;
  ~FilterGraph() { avfilter_graph_free(&graph); }
};

static void throw_av(const std::string& prefix, const int status) {
  char message[256];
  av_strerror(status, message, sizeof(message));
  throw std::runtime_error(prefix + message);
}

static FilterGraph build_graph(const int width, const int height, const std::string& filters) {
  FilterGraph built;
  built.graph = avfilter_graph_alloc();
  AVFilterInOut* outputs = avfilter_inout_alloc();
  AVFilterInOut* inputs = avfilter_inout_alloc();
  if (built.graph == nullptr || outputs == nullptr || inputs == nullptr) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&built.graph);
    throw std::runtime_error("failed to allocate filter graph");
  }

  const std::string args = "video_size=" + std::to_string(width) + "x" + std::to_string(height) + ":pix_fmt=" + std::to_string(static_cast<int>(AV_PIX_FMT_RGB24)) + ":time_base=1/1:pixel_aspect=1/1";
  int ret = avfilter_graph_create_filter(&built.src, avfilter_get_by_name("buffer"), "in", args.c_str(), nullptr, built.graph);
  if (ret < 0) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&built.graph);
    throw_av("Cannot create buffer source: ", ret);
  }

  ret = avfilter_graph_create_filter(&built.sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, built.graph);
  if (ret < 0) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&built.graph);
    throw_av("Cannot create buffer sink: ", ret);
  }

  outputs->name = av_strdup("in");
  outputs->filter_ctx = built.src;
  outputs->pad_idx = 0;
  outputs->next = nullptr;

  inputs->name = av_strdup("out");
  inputs->filter_ctx = built.sink;
  inputs->pad_idx = 0;
  inputs->next = nullptr;

  ret = avfilter_graph_parse_ptr(built.graph, filters.c_str(), &inputs, &outputs, nullptr);
  if (ret >= 0) {
    ret = avfilter_graph_config(built.graph, nullptr);
  }
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  if (ret < 0) {
    avfilter_graph_free(&built.graph);
    throw_av("filter graph failed: ", ret);
  }
  return built;
}

static std::string analysis_chain(const char* filter_name, const ScopeColorMode mode, const bool use_10_bpc, const std::string& user_options) {
  return std::string("format=") + scope_analysis_pix_fmt(mode, use_10_bpc) + "," + scope_filter_spec(filter_name, mode, user_options) + ",format=rgb24";
}

static const AVFilterLink* scope_input_link(const AVFilterGraph* graph, const char* filter_name) {
  if (graph == nullptr) {
    return nullptr;
  }
  for (unsigned i = 0; i < graph->nb_filters; ++i) {
    AVFilterContext* ctx = graph->filters[i];
    if (ctx != nullptr && ctx->filter != nullptr && std::strcmp(ctx->filter->name, filter_name) == 0 && ctx->nb_inputs > 0 && ctx->inputs != nullptr && ctx->inputs[0] != nullptr) {
      return ctx->inputs[0];
    }
  }
  return nullptr;
}

static const char* pix_fmt_name(const AVPixelFormat format) {
  const char* name = av_get_pix_fmt_name(format);
  return name != nullptr ? name : "unknown";
}

// Negotiation can insert a scale in front of a scope. The scope's input link is the format it actually analyzes.
static void expect_direct_scope_input(const FilterGraph& graph, const char* filter_name, const ScopeColorMode mode, const bool use_10_bpc, const std::string& user_options) {
  const AVPixelFormat expected = av_get_pix_fmt(scope_analysis_pix_fmt(mode, use_10_bpc));
  const AVFilterLink* link = scope_input_link(graph.graph, filter_name);
  const std::string label = std::string(filter_name) + " receives " + scope_analysis_pix_fmt(mode, use_10_bpc) + (user_options.empty() ? "" : " with " + user_options);
  if (link == nullptr || link->src == nullptr || link->src->filter == nullptr) {
    std::fprintf(stderr, "FAIL %s: scope input link missing\n", label.c_str());
    failures++;
    return;
  }
  const AVPixelFormat got = static_cast<AVPixelFormat>(link->format);
  const char* source = link->src->filter->name;
  if (got != expected || std::strcmp(source, "format") != 0) {
    std::fprintf(stderr, "FAIL %s: input %s from %s\n", label.c_str(), pix_fmt_name(got), source);
    failures++;
    return;
  }
  std::printf("PASS %s\n", label.c_str());
}

static PackedFrame run_scope(const char* filter_name, const ScopeColorMode mode, const bool use_10_bpc, const std::string& user_options, const Rgb color) {
  constexpr int kWidth = 32;
  constexpr int kHeight = 16;
  const std::string filters = analysis_chain(filter_name, mode, use_10_bpc, user_options);
  FilterGraph graph = build_graph(kWidth, kHeight, filters);
  expect_direct_scope_input(graph, filter_name, mode, use_10_bpc, user_options);

  AVFrame* input = av_frame_alloc();
  input->format = AV_PIX_FMT_RGB24;
  input->width = kWidth;
  input->height = kHeight;
  if (av_frame_get_buffer(input, 32) < 0) {
    av_frame_free(&input);
    throw std::runtime_error("av_frame_get_buffer failed");
  }
  for (int y = 0; y < kHeight; ++y) {
    uint8_t* row = input->data[0] + y * input->linesize[0];
    for (int x = 0; x < kWidth; ++x) {
      row[x * 3 + 0] = static_cast<uint8_t>(color.r);
      row[x * 3 + 1] = static_cast<uint8_t>(color.g);
      row[x * 3 + 2] = static_cast<uint8_t>(color.b);
    }
  }

  const int add_status = av_buffersrc_add_frame(graph.src, input);
  av_frame_free(&input);
  if (add_status < 0) {
    throw_av("av_buffersrc_add_frame failed: ", add_status);
  }

  AVFrame* output = av_frame_alloc();
  const int get_status = av_buffersink_get_frame(graph.sink, output);
  if (get_status < 0) {
    av_frame_free(&output);
    throw_av("av_buffersink_get_frame failed: ", get_status);
  }

  PackedFrame packed;
  packed.width = output->width;
  packed.height = output->height;
  packed.rgb.resize(static_cast<size_t>(packed.width * packed.height * 3));
  for (int y = 0; y < packed.height; ++y) {
    const uint8_t* src = output->data[0] + y * output->linesize[0];
    uint8_t* dst = packed.rgb.data() + static_cast<size_t>(y * packed.width * 3);
    for (int x = 0; x < packed.width * 3; ++x) {
      dst[x] = src[x];
    }
  }
  av_frame_free(&output);
  return packed;
}

static int histogram_spike_x(const PackedFrame& frame, const int band, const int bands) {
  const int band_h = frame.height / bands;
  const int y0 = band * band_h;
  int best_x = -1;
  int best_count = -1;
  for (int x = 0; x < frame.width; ++x) {
    int count = 0;
    for (int y = y0; y < y0 + band_h; ++y) {
      const uint8_t* p = frame.rgb.data() + static_cast<size_t>((y * frame.width + x) * 3);
      if (p[0] + p[1] + p[2] > 400) {
        ++count;
      }
    }
    if (count > best_count) {
      best_count = count;
      best_x = x;
    }
  }
  return best_x;
}

struct Trace {
  int y;
  int r;
  int g;
  int b;
};

static Trace waveform_trace(const PackedFrame& frame, const int band, const int bands) {
  const int band_h = frame.height / bands;
  const int y0 = band * band_h;
  Trace best{0, 0, 0, 0};
  int best_sum = -1;
  for (int y = 0; y < band_h; ++y) {
    const uint8_t* p = frame.rgb.data() + static_cast<size_t>((y0 + y) * frame.width * 3);
    const int sum = p[0] + p[1] + p[2];
    if (sum > best_sum) {
      best_sum = sum;
      best = {y, p[0], p[1], p[2]};
    }
  }
  return best;
}

struct Dot {
  int x;
  int y;
  int r;
  int g;
  int b;
};

static Dot brightest_dot(const PackedFrame& frame) {
  Dot best{0, 0, 0, 0, 0};
  int best_sum = -1;
  for (int y = 0; y < frame.height; ++y) {
    for (int x = 0; x < frame.width; ++x) {
      const uint8_t* p = frame.rgb.data() + static_cast<size_t>((y * frame.width + x) * 3);
      const int sum = p[0] + p[1] + p[2];
      if (sum > best_sum) {
        best_sum = sum;
        best = {x, y, p[0], p[1], p[2]};
      }
    }
  }
  return best;
}

static bool near(const int got, const int expected, const int tolerance) {
  const int delta = got - expected;
  return delta <= tolerance && delta >= -tolerance;
}

static bool high_in_band(const int y, const int band_h) {
  return y <= band_h / 16;
}

static bool low_in_band(const int y, const int band_h) {
  return y >= band_h - 1 - band_h / 16;
}

static bool red_pixel(const int r, const int g, const int b) {
  return r > g + 40 && r > b + 40;
}

static bool green_pixel(const int r, const int g, const int b) {
  return g > r + 40 && g > b + 40;
}

static bool blue_pixel(const int r, const int g, const int b) {
  return b > r + 40 && b > g + 40;
}

static void test_parsing() {
  const ScopeColorModes defaults;
  expect_modes("omitted option keeps histogram RGB and vectorscope/waveform YUV", defaults, ScopeColorMode::Rgb, ScopeColorMode::Yuv, ScopeColorMode::Yuv);

  expect_modes("single yuv applies to all scopes", parse_scope_color_modes("yuv"), ScopeColorMode::Yuv, ScopeColorMode::Yuv, ScopeColorMode::Yuv);
  expect_modes("single rgb applies to all scopes", parse_scope_color_modes("rgb"), ScopeColorMode::Rgb, ScopeColorMode::Rgb, ScopeColorMode::Rgb);

  expect_modes("yuv::rgb overrides histogram and waveform", parse_scope_color_modes("yuv::rgb"), ScopeColorMode::Yuv, ScopeColorMode::Yuv, ScopeColorMode::Rgb);
  expect_modes("rgb:yuv:rgb sets each scope", parse_scope_color_modes("rgb:yuv:rgb"), ScopeColorMode::Rgb, ScopeColorMode::Yuv, ScopeColorMode::Rgb);
  expect_modes("::rgb overrides waveform only", parse_scope_color_modes("::rgb"), ScopeColorMode::Rgb, ScopeColorMode::Yuv, ScopeColorMode::Rgb);
  expect_modes("yuv:rgb overrides histogram and vectorscope", parse_scope_color_modes("yuv:rgb"), ScopeColorMode::Yuv, ScopeColorMode::Rgb, ScopeColorMode::Yuv);
  expect_modes("trailing empty field keeps waveform default", parse_scope_color_modes("rgb:yuv:"), ScopeColorMode::Rgb, ScopeColorMode::Yuv, ScopeColorMode::Yuv);
  expect_modes("leading empty field keeps histogram default", parse_scope_color_modes(":rgb"), ScopeColorMode::Rgb, ScopeColorMode::Rgb, ScopeColorMode::Yuv);
  expect_modes("empty fields keep every default", parse_scope_color_modes("::"), ScopeColorMode::Rgb, ScopeColorMode::Yuv, ScopeColorMode::Yuv);

  expect_error("empty value is rejected", "", "Cannot parse scope color mode '' (valid options: rgb, yuv)");
  expect_error("uppercase RGB is rejected", "RGB", "Cannot parse scope color mode 'RGB' (valid options: rgb, yuv)");
  expect_error("uppercase YUV is rejected", "YUV", "Cannot parse scope color mode 'YUV' (valid options: rgb, yuv)");
  expect_error("unknown positional value is rejected", "yuv:rgba", "Cannot parse scope color mode 'rgba' (valid options: rgb, yuv)");
  expect_error("four positions are rejected", "rgb:yuv:rgb:yuv", "Cannot parse scope color mode (expected at most 3 colon-separated values)");
  expect_error("trailing colon after three values is a fourth position", "rgb:yuv:rgb:", "Cannot parse scope color mode (expected at most 3 colon-separated values)");
}

static void test_format_and_option_strings() {
  expect_eq("8-bit RGB format", scope_analysis_pix_fmt(ScopeColorMode::Rgb, false), "gbrp");
  expect_eq("8-bit YUV format", scope_analysis_pix_fmt(ScopeColorMode::Yuv, false), "yuv444p");
  expect_eq("10-bit RGB format", scope_analysis_pix_fmt(ScopeColorMode::Rgb, true), "gbrp10");
  expect_eq("10-bit YUV format", scope_analysis_pix_fmt(ScopeColorMode::Yuv, true), "yuv444p10le");

  expect_eq("default histogram spec", scope_filter_spec("histogram", ScopeColorMode::Rgb, ""), "histogram");
  expect_eq("default vectorscope spec", scope_filter_spec("vectorscope", ScopeColorMode::Yuv, ""), "vectorscope");
  expect_eq("default waveform spec stays luma-only", scope_filter_spec("waveform", ScopeColorMode::Yuv, ""), "waveform");
  expect_eq("RGB waveform shows all three channels", scope_filter_spec("waveform", ScopeColorMode::Rgb, ""), "waveform=components=7");

  const std::string histogram_options = "display_mode=parade:colors_mode=coloronblack:level_height=256:levels_mode=logarithmic";
  const std::string vectorscope_options = "mode=color4:graticule=green:envelope=instant+peak:flags=name+white+black";
  const std::string waveform_options = "graticule=orange:display=stack:scale=ire:flags=numbers+dots:intensity=0.1:components=7:filter=lowpass";

  expect_eq("histogram options pass through in RGB", scope_filter_spec("histogram", ScopeColorMode::Rgb, histogram_options), "histogram=" + histogram_options);
  expect_eq("histogram options pass through in YUV", scope_filter_spec("histogram", ScopeColorMode::Yuv, histogram_options), "histogram=" + histogram_options);
  expect_eq("vectorscope options pass through in YUV", scope_filter_spec("vectorscope", ScopeColorMode::Yuv, vectorscope_options), "vectorscope=" + vectorscope_options);
  expect_eq("vectorscope options pass through in RGB", scope_filter_spec("vectorscope", ScopeColorMode::Rgb, vectorscope_options), "vectorscope=" + vectorscope_options);
  expect_eq("explicit waveform components are kept for YUV", scope_filter_spec("waveform", ScopeColorMode::Yuv, waveform_options), "waveform=" + waveform_options);
  expect_eq("explicit waveform components are kept for RGB", scope_filter_spec("waveform", ScopeColorMode::Rgb, waveform_options), "waveform=" + waveform_options);
  expect_eq("short component option is kept", scope_filter_spec("waveform", ScopeColorMode::Rgb, "c=1"), "waveform=c=1");
  expect_eq("RGB waveform appends components when absent", scope_filter_spec("waveform", ScopeColorMode::Rgb, "graticule=orange:intensity=0.1"), "waveform=graticule=orange:intensity=0.1:components=7");
  expect_eq("YUV waveform does not append components", scope_filter_spec("waveform", ScopeColorMode::Yuv, "graticule=orange:intensity=0.1"), "waveform=graticule=orange:intensity=0.1");
}

static void test_graphs() {
  const Rgb red{255, 0, 0};
  const Rgb green{0, 255, 0};
  const Rgb blue{0, 0, 255};
  const Rgb black{0, 0, 0};
  const Rgb white{255, 255, 255};

  const PackedFrame hist_rgb = run_scope("histogram", ScopeColorMode::Rgb, false, "", red);
  expect(hist_rgb.width == 256 && hist_rgb.height % 3 == 0, "8-bit RGB histogram is accepted and shows three components");
  expect(histogram_spike_x(hist_rgb, 0, 3) >= 250 && histogram_spike_x(hist_rgb, 1, 3) <= 2 && histogram_spike_x(hist_rgb, 2, 3) <= 2, "8-bit RGB histogram of red is high in R and low in G and B");

  const PackedFrame hist_rgb_green = run_scope("histogram", ScopeColorMode::Rgb, false, "", green);
  expect(histogram_spike_x(hist_rgb_green, 0, 3) <= 2 && histogram_spike_x(hist_rgb_green, 1, 3) >= 250 && histogram_spike_x(hist_rgb_green, 2, 3) <= 2, "8-bit RGB histogram of green is high in G only");

  const PackedFrame hist_yuv_red = run_scope("histogram", ScopeColorMode::Yuv, false, "", red);
  const PackedFrame hist_yuv_blue = run_scope("histogram", ScopeColorMode::Yuv, false, "", blue);
  expect(hist_yuv_red.width == 256 && hist_yuv_red.height % 3 == 0, "8-bit YUV histogram is accepted and shows three components");
  const int yuv_red_y = histogram_spike_x(hist_yuv_red, 0, 3);
  const int yuv_red_u = histogram_spike_x(hist_yuv_red, 1, 3);
  const int yuv_red_v = histogram_spike_x(hist_yuv_red, 2, 3);
  const int yuv_blue_u = histogram_spike_x(hist_yuv_blue, 1, 3);
  expect(yuv_red_y > 40 && yuv_red_y < 180 && yuv_red_v > 180, "8-bit YUV histogram of red is luma plus high V, not an RGB triple");
  expect(std::abs(yuv_red_u - yuv_blue_u) > 80, "8-bit YUV histogram chroma differs between red and blue");

  const PackedFrame hist_rgb_10 = run_scope("histogram", ScopeColorMode::Rgb, true, "", red);
  expect(hist_rgb_10.width == 1024 && hist_rgb_10.height % 3 == 0, "10-bit RGB histogram is accepted");
  expect(histogram_spike_x(hist_rgb_10, 0, 3) >= 1000 && histogram_spike_x(hist_rgb_10, 1, 3) <= 4 && histogram_spike_x(hist_rgb_10, 2, 3) <= 4, "10-bit RGB histogram of red uses the 10-bit R bin");

  const PackedFrame hist_yuv_10 = run_scope("histogram", ScopeColorMode::Yuv, true, "", red);
  expect(hist_yuv_10.width == 1024 && hist_yuv_10.height % 3 == 0, "10-bit YUV histogram is accepted");
  expect(histogram_spike_x(hist_yuv_10, 0, 3) < 800 && histogram_spike_x(hist_yuv_10, 2, 3) > 800, "10-bit YUV histogram of red is not the RGB component pattern");

  const std::string histogram_options = "display_mode=parade:colors_mode=coloronblack:level_height=256:levels_mode=logarithmic";
  const PackedFrame hist_opts = run_scope("histogram", ScopeColorMode::Rgb, false, histogram_options, red);
  expect(hist_opts.width == 768 && hist_opts.height == 268, "histogram options select parade layout without changing color mode");
  const PackedFrame hist_opts_yuv = run_scope("histogram", ScopeColorMode::Yuv, false, histogram_options, red);
  expect(hist_opts_yuv.width == 768 && hist_opts_yuv.height == 268, "histogram options still apply in YUV mode");

  const PackedFrame vec_rgb = run_scope("vectorscope", ScopeColorMode::Rgb, false, "", red);
  const PackedFrame vec_yuv = run_scope("vectorscope", ScopeColorMode::Yuv, false, "", red);
  const Dot rgb_dot = brightest_dot(vec_rgb);
  const Dot yuv_dot = brightest_dot(vec_yuv);
  expect(vec_rgb.width == 256 && vec_rgb.height == 256, "8-bit RGB vectorscope is accepted");
  expect(vec_yuv.width == 256 && vec_yuv.height == 256, "8-bit YUV vectorscope is accepted");
  expect(near(rgb_dot.x, 0, 8) && near(rgb_dot.y, 0, 8), "8-bit RGB vectorscope plots the red sample on the B/R axes");
  expect(yuv_dot.x > 40 && yuv_dot.y < 40, "8-bit YUV vectorscope plots red chroma away from the RGB corner");

  const PackedFrame vec_rgb_blue = run_scope("vectorscope", ScopeColorMode::Rgb, false, "", blue);
  const Dot rgb_blue = brightest_dot(vec_rgb_blue);
  expect(near(rgb_blue.x, 255, 8) && near(rgb_blue.y, 255, 8), "8-bit RGB vectorscope moves to the blue sample");

  const PackedFrame vec_rgb_10 = run_scope("vectorscope", ScopeColorMode::Rgb, true, "", red);
  const PackedFrame vec_yuv_10 = run_scope("vectorscope", ScopeColorMode::Yuv, true, "", red);
  expect(vec_rgb_10.width == 1024 && vec_rgb_10.height == 1024, "10-bit RGB vectorscope is accepted");
  expect(vec_yuv_10.width == 1024 && vec_yuv_10.height == 1024, "10-bit YUV vectorscope is accepted");

  const PackedFrame vec_color4_rgb = run_scope("vectorscope", ScopeColorMode::Rgb, false, "mode=color4", red);
  const PackedFrame vec_color4_yuv = run_scope("vectorscope", ScopeColorMode::Yuv, false, "mode=color4:graticule=green:envelope=instant+peak:flags=name+white+black", red);
  const Dot color4_rgb = brightest_dot(vec_color4_rgb);
  const Dot color4_yuv = brightest_dot(vec_color4_yuv);
  expect(red_pixel(color4_rgb.r, color4_rgb.g, color4_rgb.b) && near(color4_rgb.x, 0, 8), "vectorscope mode=color4 colors the RGB dot");
  expect(red_pixel(color4_yuv.r, color4_yuv.g, color4_yuv.b) && color4_yuv.x > 40, "vectorscope options color the YUV dot without moving it to RGB");

  const PackedFrame wave_yuv_black = run_scope("waveform", ScopeColorMode::Yuv, false, "", black);
  const PackedFrame wave_yuv_white = run_scope("waveform", ScopeColorMode::Yuv, false, "", white);
  const Trace yuv_black = waveform_trace(wave_yuv_black, 0, 1);
  const Trace yuv_white = waveform_trace(wave_yuv_white, 0, 1);
  expect(wave_yuv_black.height == 256 && wave_yuv_white.height == 256, "default YUV waveform shows the single luma component");
  expect(yuv_black.y > 200 && yuv_white.y < 40, "default YUV waveform trace follows luma");

  const PackedFrame wave_rgb_red = run_scope("waveform", ScopeColorMode::Rgb, false, "", red);
  const PackedFrame wave_rgb_green = run_scope("waveform", ScopeColorMode::Rgb, false, "", green);
  expect(wave_rgb_red.height == 768, "RGB waveform shows three component bands");
  const int band_h = wave_rgb_red.height / 3;
  const Trace red_r = waveform_trace(wave_rgb_red, 0, 3);
  const Trace red_g = waveform_trace(wave_rgb_red, 1, 3);
  const Trace red_b = waveform_trace(wave_rgb_red, 2, 3);
  expect(red_pixel(red_r.r, red_r.g, red_r.b) && green_pixel(red_g.r, red_g.g, red_g.b) && blue_pixel(red_b.r, red_b.g, red_b.b), "RGB waveform draws R, G, and B in separate colors");
  expect(high_in_band(red_r.y, band_h) && low_in_band(red_g.y, band_h) && low_in_band(red_b.y, band_h), "RGB waveform of red is high only in R");

  const Trace green_r = waveform_trace(wave_rgb_green, 0, 3);
  const Trace green_g = waveform_trace(wave_rgb_green, 1, 3);
  const Trace green_b = waveform_trace(wave_rgb_green, 2, 3);
  expect(low_in_band(green_r.y, band_h) && high_in_band(green_g.y, band_h) && low_in_band(green_b.y, band_h), "RGB waveform of green is high only in G");

  const PackedFrame wave_rgb_10 = run_scope("waveform", ScopeColorMode::Rgb, true, "", red);
  expect(wave_rgb_10.height == 3072, "10-bit RGB waveform shows three 10-bit bands");
  const int band_h_10 = wave_rgb_10.height / 3;
  const Trace red_10_r = waveform_trace(wave_rgb_10, 0, 3);
  const Trace red_10_g = waveform_trace(wave_rgb_10, 1, 3);
  const Trace red_10_b = waveform_trace(wave_rgb_10, 2, 3);
  expect(red_pixel(red_10_r.r, red_10_r.g, red_10_r.b) && high_in_band(red_10_r.y, band_h_10), "10-bit RGB waveform R trace is at the top of its band");
  expect(green_pixel(red_10_g.r, red_10_g.g, red_10_g.b) && low_in_band(red_10_g.y, band_h_10) && blue_pixel(red_10_b.r, red_10_b.g, red_10_b.b) && low_in_band(red_10_b.y, band_h_10), "10-bit RGB waveform G and B traces stay low for red");

  const PackedFrame wave_yuv_10 = run_scope("waveform", ScopeColorMode::Yuv, true, "", white);
  expect(wave_yuv_10.height == 1024, "10-bit YUV waveform keeps the single luma component");

  const PackedFrame wave_explicit = run_scope("waveform", ScopeColorMode::Rgb, false, "components=1", green);
  expect(wave_explicit.height == 256, "explicit waveform components=1 is not replaced");
  const Trace explicit_trace = waveform_trace(wave_explicit, 0, 1);
  expect(red_pixel(explicit_trace.r, explicit_trace.g, explicit_trace.b) && low_in_band(explicit_trace.y, wave_explicit.height), "explicit components=1 still shows only the low red channel of green");

  const PackedFrame wave_short = run_scope("waveform", ScopeColorMode::Rgb, false, "c=1", red);
  expect(wave_short.height == 256 && red_pixel(waveform_trace(wave_short, 0, 1).r, waveform_trace(wave_short, 0, 1).g, waveform_trace(wave_short, 0, 1).b), "explicit waveform c=1 is kept");

  const PackedFrame wave_added = run_scope("waveform", ScopeColorMode::Rgb, false, "graticule=orange:intensity=0.1", red);
  expect(wave_added.height == 768, "RGB waveform options without components still show three channels");

  const std::string waveform_options = "graticule=orange:display=stack:scale=ire:flags=numbers+dots:intensity=0.1:components=7:filter=lowpass";
  const PackedFrame wave_user = run_scope("waveform", ScopeColorMode::Yuv, false, waveform_options, white);
  const PackedFrame wave_user_rgb = run_scope("waveform", ScopeColorMode::Rgb, false, waveform_options, white);
  expect(wave_user.height == 768 && wave_user_rgb.height == 768, "documented waveform options are accepted for YUV and RGB");

  // color and acolor accept planar RGB. flat, chroma, aflat, xflat, and yflat do not.
  FilterGraph color_graph = build_graph(32, 16, analysis_chain("waveform", ScopeColorMode::Rgb, false, "filter=color"));
  expect_direct_scope_input(color_graph, "waveform", ScopeColorMode::Rgb, false, "filter=color");
  FilterGraph acolor_graph = build_graph(32, 16, analysis_chain("waveform", ScopeColorMode::Rgb, false, "filter=acolor"));
  expect_direct_scope_input(acolor_graph, "waveform", ScopeColorMode::Rgb, false, "filter=acolor");

  const char* yuv_only_waveform_modes[] = {"flat", "chroma", "aflat", "xflat", "yflat"};
  for (const char* waveform_mode : yuv_only_waveform_modes) {
    const std::string options = std::string("filter=") + waveform_mode;
    FilterGraph yuv_graph = build_graph(32, 16, analysis_chain("waveform", ScopeColorMode::Yuv, false, options));
    expect_direct_scope_input(yuv_graph, "waveform", ScopeColorMode::Yuv, false, options);

    FilterGraph rgb_graph = build_graph(32, 16, analysis_chain("waveform", ScopeColorMode::Rgb, false, options));
    const AVFilterLink* link = scope_input_link(rgb_graph.graph, "waveform");
    const std::string label = std::string("RGB waveform filter=") + waveform_mode + " is converted to YUV";
    if (link == nullptr || link->src == nullptr || link->src->filter == nullptr) {
      std::fprintf(stderr, "FAIL %s: scope input link missing\n", label.c_str());
      failures++;
      continue;
    }
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(link->format));
    const bool yuv = desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_RGB) == 0;
    if (!yuv || std::strcmp(link->src->filter->name, "scale") != 0) {
      std::fprintf(stderr, "FAIL %s: input %s from %s\n", label.c_str(), pix_fmt_name(static_cast<AVPixelFormat>(link->format)), link->src->filter->name);
      failures++;
      continue;
    }
    std::printf("PASS %s -> %s\n", label.c_str(), pix_fmt_name(static_cast<AVPixelFormat>(link->format)));
  }
}

int main() {
  av_log_set_level(AV_LOG_ERROR);
  try {
    test_parsing();
    test_format_and_option_strings();
    test_graphs();
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "FAIL exception: %s\n", exception.what());
    return 1;
  }

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all scope color mode tests passed\n");
  return 0;
}
