#include "video_filterer.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include "video_filter_state.h"
extern "C" {
#include <libavutil/error.h>
}

static int failures = 0;

static void expect_pair(const char* label, const std::pair<int, int>& got, const int expected_w, const int expected_h) {
  if (got.first != expected_w || got.second != expected_h) {
    std::fprintf(stderr, "FAIL %s: expected %dx%d, got %dx%d\n", label, expected_w, expected_h, got.first, got.second);
    failures++;
    return;
  }
  std::printf("PASS %s -> %dx%d\n", label, got.first, got.second);
}

static void expect_bool(const char* label, const bool got, const bool expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: expected %d, got %d\n", label, static_cast<int>(expected), static_cast<int>(got));
    failures++;
    return;
  }
  std::printf("PASS %s -> %s\n", label, got ? "true" : "false");
}

static void expect_crop(const char* label, const CropState& got, const CropState& expected) {
  if (!video_filter_state::crop_states_equal(got, expected)) {
    std::fprintf(stderr, "FAIL %s: expected enabled=%d %dx%d+%d+%d, got enabled=%d %dx%d+%d+%d\n", label, static_cast<int>(expected.enabled), expected.rect.w, expected.rect.h, expected.rect.x, expected.rect.y,
                 static_cast<int>(got.enabled), got.rect.w, got.rect.h, got.rect.x, got.rect.y);
    failures++;
    return;
  }
  std::printf("PASS %s\n", label);
}

struct ConfiguredGraph {
  AVFilterGraph* graph{nullptr};
  AVFilterContext* src{nullptr};
  AVFilterContext* sink{nullptr};
};

static void throw_av_error(const char* prefix, const int status) {
  char message[256];
  av_strerror(status, message, sizeof(message));
  throw std::runtime_error(std::string(prefix) + message);
}

static ConfiguredGraph build_graph(const int width, const int height, const std::string& filters) {
  ConfiguredGraph built;
  built.graph = avfilter_graph_alloc();
  AVFilterInOut* outputs = avfilter_inout_alloc();
  AVFilterInOut* inputs = avfilter_inout_alloc();

  if (built.graph == nullptr || outputs == nullptr || inputs == nullptr) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&built.graph);
    throw std::runtime_error("failed to allocate filter graph");
  }

  const std::string args = "video_size=" + std::to_string(width) + "x" + std::to_string(height) + ":pix_fmt=" + std::to_string(static_cast<int>(AV_PIX_FMT_YUV420P)) + ":time_base=1/25:pixel_aspect=1/1";

  int ret = avfilter_graph_create_filter(&built.src, avfilter_get_by_name("buffer"), "in", args.c_str(), nullptr, built.graph);
  if (ret < 0) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&built.graph);
    throw_av_error("Cannot create buffer source: ", ret);
  }

  ret = avfilter_graph_create_filter(&built.sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, built.graph);
  if (ret < 0) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&built.graph);
    throw_av_error("Cannot create buffer sink: ", ret);
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
    throw_av_error("filter graph failed: ", ret);
  }

  return built;
}

static void free_graph(ConfiguredGraph& graph) {
  avfilter_graph_free(&graph.graph);
  graph.src = nullptr;
  graph.sink = nullptr;
}

static std::pair<int, int> dest_size(const ConfiguredGraph& graph) {
  return {graph.sink->inputs[0]->w, graph.sink->inputs[0]->h};
}

static std::pair<int, int> crop_space(const ConfiguredGraph& graph, const std::string& pre_filters, const int src_w, const int src_h) {
  return video_filter_state::crop_space_from_configured_chain(graph.src, graph.sink, video_filter_state::count_linear_filter_instances(pre_filters), src_w, src_h);
}

int main() {
  const int src_w = 1920;
  const int src_h = 1080;
  const CropState hd_crop{{100, 50, 1600, 900}, true};

  try {
    const std::string none = video_filter_state::compose_filters("", "", {}, false);
    ConfiguredGraph identity = build_graph(src_w, src_h, none);
    expect_pair("no filters crop-space is decoded size", crop_space(identity, "", src_w, src_h), src_w, src_h);
    expect_pair("no filters dest matches crop-space", dest_size(identity), src_w, src_h);
    free_graph(identity);

    const std::string post_only = video_filter_state::compose_filters("", "scale=1280:720", {}, false);
    ConfiguredGraph post = build_graph(src_w, src_h, post_only);
    const auto post_space = crop_space(post, "", src_w, src_h);
    const auto post_dest = dest_size(post);
    expect_pair("post-scale crop-space stays decoded size", post_space, src_w, src_h);
    expect_pair("post-scale dest is the scaled output", post_dest, 1280, 720);
    expect_bool("post-scale crop-space differs from dest", post_space != post_dest, true);

    const std::string post_crop = video_filter_state::compose_filters("", "scale=1280:720", hd_crop.rect, true);
    ConfiguredGraph post_enabled = build_graph(src_w, src_h, post_crop);
    expect_pair("post-scale crop-space with crop enabled", crop_space(post_enabled, "", src_w, src_h), src_w, src_h);
    expect_pair("crop-space is unchanged when crop is enabled", crop_space(post_enabled, "", src_w, src_h), post_space.first, post_space.second);
    expect_pair("post-scale dest with crop still follows the post-filter", dest_size(post_enabled), 1280, 720);

    const CropState mapped_in_crop_space = video_filter_state::map_crop_state(hd_crop, src_w, src_h, post_space.first, post_space.second);
    const CropState mapped_in_dest = video_filter_state::map_crop_state(hd_crop, src_w, src_h, post_dest.first, post_dest.second);
    expect_crop("copy uses crop-space, not post-filter dest", mapped_in_crop_space, hd_crop);
    expect_bool("mapping through dest output would change the rect", !video_filter_state::crop_states_equal(mapped_in_dest, hd_crop), true);
    free_graph(post);
    free_graph(post_enabled);

    const std::string pre_post = "scale=960:540";
    const CropState pre_crop{{20, 10, 400, 300}, true};
    const std::string pre_no_crop = video_filter_state::compose_filters(pre_post, "scale=640:360", {}, false);
    const std::string pre_with_crop = video_filter_state::compose_filters(pre_post, "scale=640:360", pre_crop.rect, true);
    ConfiguredGraph pre_off = build_graph(src_w, src_h, pre_no_crop);
    ConfiguredGraph pre_on = build_graph(src_w, src_h, pre_with_crop);
    expect_pair("pre-scale crop-space without crop", crop_space(pre_off, pre_post, src_w, src_h), 960, 540);
    expect_pair("pre-scale crop-space with crop", crop_space(pre_on, pre_post, src_w, src_h), 960, 540);
    expect_pair("pre+post dest without crop is post-filter output", dest_size(pre_off), 640, 360);
    expect_bool("pre-scale crop-space is independent of crop enabled", crop_space(pre_off, pre_post, src_w, src_h) == crop_space(pre_on, pre_post, src_w, src_h), true);
    free_graph(pre_off);
    free_graph(pre_on);

    const std::string transpose = "transpose=clock";
    const std::string transpose_filters = video_filter_state::compose_filters(transpose, "scale=640:360", {}, false);
    ConfiguredGraph rotated = build_graph(src_w, src_h, transpose_filters);
    expect_pair("transpose pre-filter swaps crop-space", crop_space(rotated, transpose, src_w, src_h), src_h, src_w);
    expect_pair("transpose dest still follows the post-filter", dest_size(rotated), 640, 360);
    free_graph(rotated);

    const std::string pre_only = video_filter_state::compose_filters("scale=1280:720", "", {}, false);
    ConfiguredGraph pre = build_graph(src_w, src_h, pre_only);
    expect_pair("pre-scale without post matches dest", crop_space(pre, "scale=1280:720", src_w, src_h), 1280, 720);
    expect_pair("pre-scale dest", dest_size(pre), 1280, 720);
    free_graph(pre);
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "FAIL graph setup: %s\n", exception.what());
    return EXIT_FAILURE;
  }

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }

  std::printf("All video filterer crop-space tests passed\n");
  return EXIT_SUCCESS;
}
