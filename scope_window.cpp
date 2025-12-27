#include "scope_window.h"
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
}
#include <SDL2/SDL.h>

void ScopeWindow::route_events(std::array<std::unique_ptr<ScopeWindow>, ScopeWindow::kNumScopes>& windows) {
  std::vector<SDL_Event> deferred_events;
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    bool consumed_by_scope = false;
    for (auto& w : windows) {
      if (w && w->handle_event(event)) {
        consumed_by_scope = true;
      }
    }

    for (auto& w : windows) {
      if (w && w->close_requested()) {
        w.reset();
        consumed_by_scope = true;
      }
    }

    if (!consumed_by_scope) {
      deferred_events.push_back(event);
    }
  }
  // Requeue events for the main display input to process
  for (auto it = deferred_events.rbegin(); it != deferred_events.rend(); ++it) {
    SDL_PushEvent(&(*it));
  }
}

namespace {
struct ScopeInfo {
  const char* title;
  const char* filter_name;  // FFmpeg runtime filter name
};

static constexpr ScopeInfo SCOPE_INFOS[] = {
    {"Histogram", "histogram"},
    {"Vectorscope", "vectorscope"},
    {"Waveform", "waveform"},
};

inline ScopeInfo get_scope_info(ScopeWindow::Type type) {
  const size_t index = static_cast<size_t>(type);
  if (index < (sizeof(SCOPE_INFOS) / sizeof(SCOPE_INFOS[0]))) {
    return SCOPE_INFOS[index];
  } else {
    return {"Scope", "histogram"};
  }
}
}  // namespace

static void sdl_check_bool(const bool ok, const char* what) {
  if (!ok) {
    throw std::runtime_error(std::string("SDL error in ") + what + ": " + SDL_GetError());
  }
}

static void* sdl_check_ptr(void* ptr, const char* what) {
  if (!ptr) {
    throw std::runtime_error(std::string("SDL error in ") + what + ": " + SDL_GetError());
  }
  return ptr;
}

static void ffmpeg_check(const int error_code, const char* what) {
  if (error_code < 0) {
    throw std::runtime_error(std::string("FFmpeg error in ") + what);
  }
}

ScopeWindow::ScopeWindow(Type type, int pane_width, int pane_height, bool always_on_top, int display_number, bool use_10_bpc)
    : type_(type), pane_width_(pane_width), pane_height_(pane_height), always_on_top_(always_on_top), display_number_(display_number), use_10_bpc_(use_10_bpc) {
  const int window_width = pane_width_ * 2;
  const int window_height = pane_height_;
  Uint32 window_flags = SDL_WINDOW_SHOWN;
  window_flags |= SDL_WINDOW_RESIZABLE;
#ifdef SDL_WINDOW_ALWAYS_ON_TOP
  if (always_on_top_) {
    window_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
  }
#endif
  // Determine initial position based on display usable bounds and tool type index to avoid overlap.
  int initial_position_x = SDL_WINDOWPOS_UNDEFINED_DISPLAY(display_number_);
  int initial_position_y = SDL_WINDOWPOS_UNDEFINED_DISPLAY(display_number_);
  SDL_Rect usable_bounds;
  if (SDL_GetDisplayUsableBounds(display_number_, &usable_bounds) == 0) {
    const int margin_pixels = 32;
    const int offset_step_pixels = 64;
    const int tool_type_index = static_cast<int>(ScopeWindow::index(type_));
    int proposed_x = usable_bounds.x + margin_pixels + tool_type_index * offset_step_pixels;
    int proposed_y = usable_bounds.y + margin_pixels + tool_type_index * offset_step_pixels;
    // Clamp to ensure the window is entirely within the usable bounds
    const int max_x = usable_bounds.x + std::max(0, usable_bounds.w - window_width - margin_pixels);
    const int max_y = usable_bounds.y + std::max(0, usable_bounds.h - window_height - margin_pixels);
    initial_position_x = std::min(std::max(proposed_x, usable_bounds.x + margin_pixels), max_x);
    initial_position_y = std::min(std::max(proposed_y, usable_bounds.y + margin_pixels), max_y);
  }

  const ScopeInfo scope_info_title = get_scope_info(type_);
  const char* window_title = scope_info_title.title;

  window_ = static_cast<SDL_Window*>(sdl_check_ptr(SDL_CreateWindow(window_title,
                           initial_position_x, initial_position_y, window_width, window_height, window_flags),
                       "SDL_CreateWindow"));

#if !defined(SDL_WINDOW_ALWAYS_ON_TOP)
  if (always_on_top_) {
    SDL_SetWindowAlwaysOnTop(window_, SDL_TRUE);
  }
#endif
  SDL_SetWindowResizable(window_, SDL_TRUE);

  renderer_ =
      static_cast<SDL_Renderer*>(sdl_check_ptr(SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC), "SDL_CreateRenderer"));
  sdl_check_bool(SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255) == 0, "SDL_SetRenderDrawColor");
  sdl_check_bool(SDL_RenderClear(renderer_) == 0, "SDL_RenderClear");
  SDL_RenderPresent(renderer_);

  window_width_ = window_width;
  window_height_ = window_height;
  window_id_ = SDL_GetWindowID(window_);
}

ScopeWindow::~ScopeWindow() {
  destroy_graph();
  if (texture_ != nullptr) {
    SDL_DestroyTexture(texture_);
    texture_ = nullptr;
  }
  if (renderer_ != nullptr) {
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
  }
  if (window_ != nullptr) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
}

std::string ScopeWindow::format_filter_args(const AVFrame* frame) {
#if (LIBAVFILTER_VERSION_INT < AV_VERSION_INT(10, 1, 100))
  return std::string("video_size=") + std::to_string(frame->width) + "x" + std::to_string(frame->height) + ":pix_fmt=" + std::to_string(frame->format) +
         ":time_base=1/1:pixel_aspect=0/1";
#else
  return std::string("video_size=") + std::to_string(frame->width) + "x" + std::to_string(frame->height) + ":pix_fmt=" + std::to_string(frame->format) +
         ":time_base=1/1:pixel_aspect=0/1:colorspace=" + std::to_string(frame->colorspace) + ":range=" + std::to_string(frame->color_range);
#endif
}

std::string ScopeWindow::build_filter_description(int pane_width,
                                                  int pane_height,
                                                  int left_colorspace,
                                                  int left_range,
                                                  int right_colorspace,
                                                  int right_range) const {
  const char* tool_name = get_scope_info(type_).filter_name;

  // Choose higher-precision input formats for the tool when 10-bpc is enabled
  const char* pre_format_filter = nullptr;
  if (type_ == Type::Histogram) {
     pre_format_filter = use_10_bpc_ ? "format=gbrp10" : "format=gbrp";
  } else {
     pre_format_filter = use_10_bpc_ ? "format=yuv444p10le" : "format=yuv444p";
  }
  const std::string setparams_left = std::string("setparams=colorspace=") + std::to_string(left_colorspace) + ":range=" + std::to_string(left_range);
  const std::string setparams_right = std::string("setparams=colorspace=") + std::to_string(right_colorspace) + ":range=" + std::to_string(right_range);
  const std::string pane_scale = std::string("scale=") + std::to_string(pane_width) + ":" + std::to_string(pane_height);

  // Final output is rgb24 for efficient SDL upload
  const char* final_format = "format=rgb24";

  std::string filter_description =
      std::string("[in_left]") + setparams_left + "," + pre_format_filter + "," + tool_name + "," + pane_scale + "[left_scope];" +
      "[in_right]" + setparams_right + "," + pre_format_filter + "," + tool_name + "," + pane_scale + "[right_scope];" +
      "[left_scope][right_scope]hstack=inputs=2," + final_format + "[out]";

  return filter_description;
}

void ScopeWindow::destroy_graph() {
  if (filter_graph_ != nullptr) {
    avfilter_graph_free(&filter_graph_);
    filter_graph_ = nullptr;
    buffersrc_left_ctx_ = nullptr;
    buffersrc_right_ctx_ = nullptr;
    buffersink_ctx_ = nullptr;
  }
}

void ScopeWindow::ensure_graph(const AVFrame* left_frame, const AVFrame* right_frame) {
  bool must_reinitialize =
      left_frame->width != input_width_left_ || left_frame->height != input_height_left_ || left_frame->format != input_format_left_ || right_frame->width != input_width_right_ ||
      right_frame->height != input_height_right_ || right_frame->format != input_format_right_ || left_frame->colorspace != input_colorspace_left_ ||
      left_frame->color_range != input_range_left_ || right_frame->colorspace != input_colorspace_right_ || right_frame->color_range != input_range_right_;

  if (!must_reinitialize && filter_graph_ != nullptr) {
    return;
  }

  destroy_graph();

  input_width_left_ = left_frame->width;
  input_height_left_ = left_frame->height;
  input_format_left_ = left_frame->format;
  input_width_right_ = right_frame->width;
  input_height_right_ = right_frame->height;
  input_format_right_ = right_frame->format;
  input_colorspace_left_ = left_frame->colorspace;
  input_range_left_ = left_frame->color_range;
  input_colorspace_right_ = right_frame->colorspace;
  input_range_right_ = right_frame->color_range;

  const AVFilter* buffersrc = avfilter_get_by_name("buffer");
  const AVFilter* buffersink = avfilter_get_by_name("buffersink");
  filter_graph_ = avfilter_graph_alloc();
  if (!filter_graph_) {
    throw std::runtime_error("avfilter_graph_alloc failed");
  }

  ffmpeg_check(avfilter_graph_create_filter(&buffersrc_left_ctx_, buffersrc, "in_left", format_filter_args(left_frame).c_str(), nullptr, filter_graph_), "create left buffer");
  ffmpeg_check(avfilter_graph_create_filter(&buffersrc_right_ctx_, buffersrc, "in_right", format_filter_args(right_frame).c_str(), nullptr, filter_graph_), "create right buffer");
  ffmpeg_check(avfilter_graph_create_filter(&buffersink_ctx_, buffersink, "out", nullptr, nullptr, filter_graph_), "create sink");

  std::string filter_description =
      build_filter_description(pane_width_, pane_height_, input_colorspace_left_, input_range_left_, input_colorspace_right_, input_range_right_);

  AVFilterInOut* inputs = avfilter_inout_alloc();
  AVFilterInOut* outputs_left = avfilter_inout_alloc();
  AVFilterInOut* outputs_right = avfilter_inout_alloc();
  if (!inputs || !outputs_left || !outputs_right) {
    throw std::runtime_error("avfilter_inout_alloc failed");
  }

  inputs->name = av_strdup("out");
  inputs->filter_ctx = buffersink_ctx_;
  inputs->pad_idx = 0;
  inputs->next = nullptr;

  outputs_left->name = av_strdup("in_left");
  outputs_left->filter_ctx = buffersrc_left_ctx_;
  outputs_left->pad_idx = 0;
  outputs_left->next = outputs_right;

  outputs_right->name = av_strdup("in_right");
  outputs_right->filter_ctx = buffersrc_right_ctx_;
  outputs_right->pad_idx = 0;
  outputs_right->next = nullptr;

  ffmpeg_check(avfilter_graph_parse_ptr(filter_graph_, filter_description.c_str(), &inputs, &outputs_left, nullptr), "graph parse");
  ffmpeg_check(avfilter_graph_config(filter_graph_, nullptr), "graph config");

  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs_left);
  // outputs_right is freed via the chained free of outputs_left->next

  if (texture_ != nullptr) {
    SDL_DestroyTexture(texture_);
    texture_ = nullptr;
  }

  texture_ = static_cast<SDL_Texture*>(sdl_check_ptr(SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, pane_width_ * 2, pane_height_), "SDL_CreateTexture"));
}

void ScopeWindow::present_frame(const AVFrame* filtered_frame) {
  sdl_check_bool(SDL_UpdateTexture(texture_, nullptr, filtered_frame->data[0], filtered_frame->linesize[0]) == 0, "SDL_UpdateTexture");
  sdl_check_bool(SDL_RenderClear(renderer_) == 0, "SDL_RenderClear");
  sdl_check_bool(SDL_RenderCopy(renderer_, texture_, nullptr, nullptr) == 0, "SDL_RenderCopy");
  SDL_RenderPresent(renderer_);
}

bool ScopeWindow::update(const AVFrame* left_frame, const AVFrame* right_frame) {
  if (!left_frame || !right_frame) {
    return false;
  }

  // If the user is browsing backward or jumping around (non-monotonic PTS),
  // reset the filter graph to avoid stale frames lingering in internal FIFOs.
  if ((last_pts_left_ != INT64_MIN && left_frame->pts < last_pts_left_) || (last_pts_right_ != INT64_MIN && right_frame->pts < last_pts_right_)) {
    destroy_graph();
  }

  ensure_graph(left_frame, right_frame);

  // Drain any pending frames to prevent lag and ensure one-to-one updates
  for (;;) {
    AVFrame* pending_frame = av_frame_alloc();
    if (!pending_frame) {
      throw std::runtime_error("av_frame_alloc failed (pending)");
    }
    const int pending_result = av_buffersink_get_frame(buffersink_ctx_, pending_frame);
    av_frame_free(&pending_frame);
    if (pending_result != 0) {
      break;
    }
  }

  // Use synthetic, matched PTS for framesync to pair both inputs
  AVFrame* left_clone = av_frame_clone(const_cast<AVFrame*>(left_frame));
  AVFrame* right_clone = av_frame_clone(const_cast<AVFrame*>(right_frame));
  if (!left_clone || !right_clone) {
    if (left_clone) {
      av_frame_free(&left_clone);
    }
    if (right_clone) {
      av_frame_free(&right_clone);
    }
    throw std::runtime_error("av_frame_clone failed");
  }
  left_clone->pts = frame_counter_;
  right_clone->pts = frame_counter_;
  ffmpeg_check(av_buffersrc_add_frame_flags(buffersrc_left_ctx_, left_clone, AV_BUFFERSRC_FLAG_KEEP_REF), "feed left frame");
  ffmpeg_check(av_buffersrc_add_frame_flags(buffersrc_right_ctx_, right_clone, AV_BUFFERSRC_FLAG_KEEP_REF), "feed right frame");
  av_frame_free(&left_clone);
  av_frame_free(&right_clone);

  // Retrieve the freshest available frame, if any
  AVFrame* latest_frame = nullptr;
  for (;;) {
    AVFrame* candidate_frame = av_frame_alloc();
    if (!candidate_frame) {
      throw std::runtime_error("av_frame_alloc failed");
    }
    const int receive_result = av_buffersink_get_frame(buffersink_ctx_, candidate_frame);
    if (receive_result != 0) {
      av_frame_free(&candidate_frame);
      break;
    }
    if (latest_frame != nullptr) {
      av_frame_free(&latest_frame);
    }
    latest_frame = candidate_frame;
  }

  if (latest_frame != nullptr) {
    present_frame(latest_frame);
    av_frame_free(&latest_frame);
    frame_counter_++;
    last_pts_left_ = left_frame->pts;
    last_pts_right_ = right_frame->pts;
    return true;
  }

  return false;
}

bool ScopeWindow::handle_event(const SDL_Event& event) {
  // Only process events directed to this window
  Uint32 event_window_id = 0;
  switch (event.type) {
    case SDL_WINDOWEVENT:
      event_window_id = event.window.windowID;
      break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
      event_window_id = event.button.windowID;
      break;
    case SDL_MOUSEMOTION:
      event_window_id = event.motion.windowID;
      break;
    case SDL_MOUSEWHEEL:
      event_window_id = event.wheel.windowID;
      break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
      event_window_id = event.key.windowID;
      break;
    default:
      break;
  }

  if (event_window_id == 0 || event_window_id != window_id_) {
    return false;
  }

  switch (event.type) {
    case SDL_WINDOWEVENT:
      if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
        close_requested_ = true;
        return true;  // consume
      } else if (event.window.event == SDL_WINDOWEVENT_RESIZED || event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        // Update internal sizing and reinitialize resources to match
        const int new_width = event.window.data1;
        const int new_height = event.window.data2;
        if (new_width > 0 && new_height > 0) {
          window_width_ = new_width;
          window_height_ = new_height;
          pane_width_ = std::max(1, window_width_ / 2);
          pane_height_ = std::max(1, window_height_);
          // Recreate resources on next update
          if (texture_ != nullptr) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
          }
          destroy_graph();
        }
        return true;  // consume
      } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
        SDL_RaiseWindow(window_);
        return true;  // consume
      }
      break;
    case SDL_MOUSEBUTTONDOWN:
      SDL_RaiseWindow(window_);
      return true;  // consume
    case SDL_MOUSEBUTTONUP:
      return true;  // consume
    case SDL_MOUSEMOTION:
      return true;  // consume
    case SDL_MOUSEWHEEL:
      return true;  // consume
    case SDL_KEYDOWN:
    case SDL_KEYUP:
      // Do not consume keyboard events; let the main app handle hotkeys
      return false;
    default:
      break;
  }

  // For other events to this window that we do not explicitly handle, do not consume by default
  return false;
}


