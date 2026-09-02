#define SDL_MAIN_HANDLED
#include "video_filterer.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include "video_filter_state.h"
#include "video_filterer_classify.h"
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

static bool crop_supported(const ConfiguredGraph& graph, const std::string& pre_filters, const bool app_crop_enabled) {
  return video_filterer_detail::interactive_crop_supported_from_configured_chain(graph.src, graph.sink, video_filter_state::count_linear_filter_instances(pre_filters), app_crop_enabled);
}

static bool try_build_graph(const int width, const int height, const std::string& filters, ConfiguredGraph* built) {
  try {
    *built = build_graph(width, height, filters);
    return true;
  } catch (const std::exception& exception) {
    std::printf("SKIP graph '%s': %s\n", filters.c_str(), exception.what());
    return false;
  }
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

    const std::string scale_post = video_filter_state::compose_filters("", "scale=1280:720", {}, false);
    const std::string scale_post_crop = video_filter_state::compose_filters("", "scale=1280:720", hd_crop.rect, true);
    ConfiguredGraph scale_off = build_graph(src_w, src_h, scale_post);
    ConfiguredGraph scale_on = build_graph(src_w, src_h, scale_post_crop);
    expect_bool("post scale is supported without app crop", crop_supported(scale_off, "", false), true);
    expect_bool("post scale is supported with app crop", crop_supported(scale_on, "", true), true);
    expect_bool("app crop does not change scale support", crop_supported(scale_off, "", false) == crop_supported(scale_on, "", true), true);
    free_graph(scale_off);
    free_graph(scale_on);

    ConfiguredGraph format_g = build_graph(src_w, src_h, video_filter_state::compose_filters("", "format=yuv420p", {}, false));
    expect_bool("post format is supported", crop_supported(format_g, "", false), true);
    free_graph(format_g);

    ConfiguredGraph setsar_g = build_graph(src_w, src_h, video_filter_state::compose_filters("", "setsar=1", {}, false));
    expect_bool("post setsar is supported", crop_supported(setsar_g, "", false), true);
    free_graph(setsar_g);

    ConfiguredGraph hflip_g = build_graph(src_w, src_h, video_filter_state::compose_filters("", "hflip", {}, false));
    ConfiguredGraph hflip_crop = build_graph(src_w, src_h, video_filter_state::compose_filters("", "hflip", hd_crop.rect, true));
    expect_bool("post hflip is unsupported", crop_supported(hflip_g, "", false), false);
    expect_bool("app crop does not change hflip support", crop_supported(hflip_g, "", false) == crop_supported(hflip_crop, "", true), true);
    free_graph(hflip_g);
    free_graph(hflip_crop);

    ConfiguredGraph transpose_post = build_graph(src_w, src_h, video_filter_state::compose_filters("", "transpose=clock", {}, false));
    expect_bool("post transpose is unsupported", crop_supported(transpose_post, "", false), false);
    free_graph(transpose_post);

    ConfiguredGraph pad_g = build_graph(src_w, src_h, video_filter_state::compose_filters("", "pad=2000:1200:40:60", {}, false));
    expect_bool("post pad is unsupported", crop_supported(pad_g, "", false), false);
    free_graph(pad_g);

    ConfiguredGraph user_crop = build_graph(src_w, src_h, video_filter_state::compose_filters("", "crop=100:80:10:20", {}, false));
    expect_bool("post user crop is unsupported", crop_supported(user_crop, "", false), false);
    free_graph(user_crop);

    ConfiguredGraph eq_g = build_graph(src_w, src_h, video_filter_state::compose_filters("", "eq=contrast=1", {}, false));
    expect_bool("unknown same-size eq is supported", crop_supported(eq_g, "", false), true);
    free_graph(eq_g);

    ConfiguredGraph zscale_g;
    if (avfilter_get_by_name("zscale") != nullptr && try_build_graph(src_w, src_h, video_filter_state::compose_filters("", "zscale=w=1280:h=720", {}, false), &zscale_g)) {
      expect_bool("post zscale is supported", crop_supported(zscale_g, "", false), true);
      free_graph(zscale_g);
    } else {
      std::printf("SKIP zscale not available\n");
    }

    ConfiguredGraph tile_g;
    if (try_build_graph(src_w, src_h, video_filter_state::compose_filters("", "tile=2x1", {}, false), &tile_g)) {
      expect_bool("unknown dimension-changing tile is unsupported", crop_supported(tile_g, "", false), false);
      free_graph(tile_g);
    }

    const std::string pre_clock = "transpose=clock";
    ConfiguredGraph pre_clock_g = build_graph(src_w, src_h, video_filter_state::compose_filters(pre_clock, "", {}, false));
    expect_pair("pre transpose crop-space is 1080x1920", crop_space(pre_clock_g, pre_clock, src_w, src_h), src_h, src_w);
    expect_bool("pre transpose does not clamp crop-space to decoded src", crop_space(pre_clock_g, pre_clock, src_w, src_h) != std::make_pair(src_w, src_h), true);
    expect_bool("empty post after pre transpose is supported", crop_supported(pre_clock_g, pre_clock, false), true);
    const CropState pre_clock_sel = video_filter_state::map_display_rect_to_crop_space({0, 0, src_h, src_w}, src_h, src_w, {}, src_h, src_w);
    expect_crop("pre-transpose geometry uses full crop-space", pre_clock_sel, CropState{{0, 0, src_h, src_w}, true});
    free_graph(pre_clock_g);

    ConfiguredGraph branch_g;
    if (try_build_graph(src_w, src_h, "split[a][b];[a][b]hstack", &branch_g)) {
      expect_bool("non-linear post chain is unsupported", crop_supported(branch_g, "", false), false);
      free_graph(branch_g);
    }
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
