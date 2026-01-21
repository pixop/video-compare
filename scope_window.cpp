#include "scope_window.h"
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "string_utils.h"
extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
}
#include <SDL2/SDL.h>

namespace {
constexpr int kFrameThickness = 2;
constexpr int kDividerThickness = 3;
constexpr int kOuterPad = kFrameThickness + kDividerThickness;  // frame + breathing room
constexpr int kHalfDividerThickness = kDividerThickness / 2;
constexpr int kHalfOuterPad = kOuterPad / 2;
constexpr int kDividerGap = kOuterPad * 2;  // total empty gap between panes

struct ScopeInfoEntry {
  ScopeWindow::Type type;
  const char* title;
  const char* filter_name;  // FFmpeg runtime filter name
  const char* type_string;  // used for logging/CLI-facing messages
};

static constexpr ScopeInfoEntry kScopeInfos[] = {
    {ScopeWindow::Type::Histogram, "Histogram", "histogram", "histogram"},
    {ScopeWindow::Type::Vectorscope, "Vectorscope", "vectorscope", "vectorscope"},
    {ScopeWindow::Type::Waveform, "Waveform", "waveform", "waveform"},
};

inline const ScopeInfoEntry* find_scope_info(const ScopeWindow::Type type) {
  for (const auto& entry : kScopeInfos) {
    if (entry.type == type) {
      return &entry;
    }
  }
  return nullptr;
}

inline const ScopeInfoEntry& get_scope_info(const ScopeWindow::Type type) {
  if (const auto* info = find_scope_info(type)) {
    return *info;
  }
  return kScopeInfos[0];
}

inline ScopeWindow::Roi clamp_roi_to_frame(const ScopeWindow::Roi& roi, const int frame_w, const int frame_h) {
  if (frame_w <= 0 || frame_h <= 0) {
    return {0, 0, 0, 0};
  }
  // Make sure crop width/height are positive and within bounds.
  const int x = std::max(0, std::min(roi.x, frame_w - 1));
  const int y = std::max(0, std::min(roi.y, frame_h - 1));
  const int w = std::max(1, std::min(roi.w, frame_w - x));
  const int h = std::max(1, std::min(roi.h, frame_h - y));
  return {x, y, w, h};
}

static void draw_rect_thickness(SDL_Renderer* renderer, const SDL_Rect& rect, const int thickness) {
  if (!renderer || thickness <= 0) {
    return;
  }
  SDL_Rect r = rect;
  for (int i = 0; i < thickness; ++i) {
    SDL_RenderDrawRect(renderer, &r);
    r.x += 1;
    r.y += 1;
    r.w = std::max(0, r.w - 2);
    r.h = std::max(0, r.h - 2);
    if (r.w <= 0 || r.h <= 0) {
      break;
    }
  }
}

static void draw_left_right_overlay(SDL_Renderer* renderer, const int w, const int h, const int divider_x) {
  if (!renderer || w <= 1 || h <= 1) {
    return;
  }

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

  const int x = std::min(std::max(divider_x, 0), w - 1);

  SDL_Rect left_rect{0, 0, x, h};
  SDL_Rect right_rect{x, 0, w - x, h};

  SDL_SetRenderDrawColor(renderer, 255, 128, 128, 255);
  draw_rect_thickness(renderer, left_rect, kFrameThickness);

  SDL_SetRenderDrawColor(renderer, 128, 128, 255, 255);
  draw_rect_thickness(renderer, right_rect, kFrameThickness);

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 140);
  for (int dx = -kHalfOuterPad; dx <= kHalfOuterPad; ++dx) {
    const int xi = x + dx;
    if (xi >= 0 && xi < w) {
      SDL_RenderDrawLine(renderer, xi, 0, xi, h - 1);
    }
  }
  SDL_SetRenderDrawColor(renderer, 255, 255, 255, 220);
  for (int dx = -kHalfDividerThickness; dx <= kHalfDividerThickness; ++dx) {
    const int xi = x + dx;
    if (xi >= 0 && xi < w) {
      SDL_RenderDrawLine(renderer, xi, 0, xi, h - 1);
    }
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

ScopeWindow::ScopeWindow(const Type type, const int pane_width, const int pane_height, const bool always_on_top, const int display_number, const bool use_10_bpc, const std::string& filter_options)
    : type_(type), pane_width_(pane_width), pane_height_(pane_height), always_on_top_(always_on_top), display_number_(display_number), use_10_bpc_(use_10_bpc), filter_options_(filter_options) {
  const int window_width = pane_width_ * 2;
  const int window_height = pane_height_;

  Uint32 window_flags = SDL_WINDOW_SHOWN;
  window_flags |= SDL_WINDOW_RESIZABLE;

  if (always_on_top_) {
    window_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
  }

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

  const auto& scope_info_title = get_scope_info(type_);
  const char* window_title = scope_info_title.title;
  base_title_ = window_title;
  last_window_title_ = window_title;

  window_ = static_cast<SDL_Window*>(sdl_check_ptr(SDL_CreateWindow(window_title, initial_position_x, initial_position_y, window_width, window_height, window_flags), "SDL_CreateWindow"));

  renderer_ = static_cast<SDL_Renderer*>(sdl_check_ptr(SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC), "SDL_CreateRenderer"));

  present_frame(nullptr);

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

const char* ScopeWindow::type_to_string(const Type t) {
  if (const auto* info = find_scope_info(t)) {
    return info->type_string;
  }
  return "scope";
}

size_t ScopeWindow::index(const Type t) {
  for (size_t i = 0; i < kNumScopes; ++i) {
    if (kScopeInfos[i].type == t) {
      return i;
    }
  }
  return 0;
}

ScopeWindow::Type ScopeWindow::type_for_index(const size_t idx) {
  if (idx < kNumScopes) {
    return kScopeInfos[idx].type;
  }
  return Type::Histogram;
}

std::array<ScopeWindow::Type, ScopeWindow::kNumScopes> ScopeWindow::all_types() {
  std::array<Type, kNumScopes> result{};
  for (size_t i = 0; i < kNumScopes; ++i) {
    result[i] = kScopeInfos[i].type;
  }
  return result;
}

std::string ScopeWindow::format_filter_args(const AVFrame* frame) {
#if (LIBAVFILTER_VERSION_INT < AV_VERSION_INT(10, 1, 100))
  return std::string("video_size=") + std::to_string(frame->width) + "x" + std::to_string(frame->height) + ":pix_fmt=" + std::to_string(frame->format) + ":time_base=1/1:pixel_aspect=0/1";
#else
  return std::string("video_size=") + std::to_string(frame->width) + "x" + std::to_string(frame->height) + ":pix_fmt=" + std::to_string(frame->format) + ":time_base=1/1:pixel_aspect=0/1:colorspace=" + std::to_string(frame->colorspace) +
         ":range=" + std::to_string(frame->color_range);
#endif
}

std::string ScopeWindow::build_filter_description(const int pane_width, const int pane_height, const int left_colorspace, const int left_range, const int right_colorspace, const int right_range, const bool roi_enabled, const Roi& roi)
    const {
  const std::string setparams_left = string_sprintf("setparams=colorspace=%d:range=%d", left_colorspace, left_range);
  const std::string setparams_right = string_sprintf("setparams=colorspace=%d:range=%d", right_colorspace, right_range);

  const int content_w = std::max(1, pane_width - kDividerGap);
  const int content_h = std::max(1, pane_height - kDividerGap);

  const std::string left_scale = string_sprintf("scale=%d:%d", content_w, content_h);
  const std::string right_scale = string_sprintf("scale=%d:%d", content_w, content_h);
  const std::string left_pad = string_sprintf("pad=%d:%d:%d:%d:color=black", pane_width, pane_height, kOuterPad, kOuterPad);
  const std::string right_pad = string_sprintf("pad=%d:%d:%d:%d:color=black", pane_width, pane_height, kOuterPad, kOuterPad);

  const std::string crop_left = roi_enabled ? string_sprintf("crop=%d:%d:%d:%d,", roi.w, roi.h, roi.x, roi.y) : "";
  const std::string crop_right = crop_left;

  const char* pre_format_filter;
  if (type_ == Type::Histogram) {
    pre_format_filter = use_10_bpc_ ? "format=gbrp10" : "format=gbrp";
  } else {
    pre_format_filter = use_10_bpc_ ? "format=yuv444p10le" : "format=yuv444p";
  }

  const char* tool_name = get_scope_info(type_).filter_name;
  const std::string tool_spec = filter_options_.empty() ? std::string(tool_name) : string_sprintf("%s=%s", tool_name, filter_options_.c_str());

  std::string filter_description = string_sprintf(
      "[in_left]%s,%s%s,%s,%s,%s[left_scope];"
      "[in_right]%s,%s%s,%s,%s,%s[right_scope];"
      "[left_scope][right_scope]hstack=inputs=2,%s[out]",
      setparams_left.c_str(), crop_left.c_str(), pre_format_filter, tool_spec.c_str(), left_scale.c_str(), left_pad.c_str(), setparams_right.c_str(), crop_right.c_str(), pre_format_filter, tool_spec.c_str(), right_scale.c_str(),
      right_pad.c_str(), "format=rgb24");

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
  Roi roi_local;
  bool roi_enabled_local = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    roi_local = roi_;
    roi_enabled_local = roi_enabled_;
  }

  Roi roi_effective{0, 0, 0, 0};
  if (roi_enabled_local) {
    roi_effective = clamp_roi_to_frame(roi_local, left_frame->width, left_frame->height);
    roi_enabled_local = roi_effective.w > 0 && roi_effective.h > 0;
  }

  bool must_reinitialize = left_frame->width != left_input_.width || left_frame->height != left_input_.height || left_frame->format != left_input_.format || right_frame->width != right_input_.width ||
                           right_frame->height != right_input_.height || right_frame->format != right_input_.format || left_frame->colorspace != left_input_.colorspace || left_frame->color_range != left_input_.range ||
                           right_frame->colorspace != right_input_.colorspace || right_frame->color_range != right_input_.range;

  // Trigger rebuild when ROI changes
  const bool roi_changed = roi_enabled_local && (prev_roi_.x != roi_effective.x || prev_roi_.y != roi_effective.y || prev_roi_.w != roi_effective.w || prev_roi_.h != roi_effective.h);

  must_reinitialize = must_reinitialize || roi_changed;

  if (!must_reinitialize && filter_graph_ != nullptr) {
    return;
  }

  destroy_graph();

  left_input_.width = left_frame->width;
  left_input_.height = left_frame->height;
  left_input_.format = left_frame->format;
  right_input_.width = right_frame->width;
  right_input_.height = right_frame->height;
  right_input_.format = right_frame->format;
  left_input_.colorspace = left_frame->colorspace;
  left_input_.range = left_frame->color_range;
  right_input_.colorspace = right_frame->colorspace;
  right_input_.range = right_frame->color_range;
  prev_roi_ = roi_enabled_local ? roi_effective : Roi{-1, -1, -1, -1};
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    source_width_ = left_frame->width;
    source_height_ = left_frame->height;
  }

  const AVFilter* buffersrc = avfilter_get_by_name("buffer");
  const AVFilter* buffersink = avfilter_get_by_name("buffersink");
  filter_graph_ = avfilter_graph_alloc();
  if (!filter_graph_) {
    throw std::runtime_error("avfilter_graph_alloc failed");
  }

  ffmpeg_check(avfilter_graph_create_filter(&buffersrc_left_ctx_, buffersrc, "in_left", format_filter_args(left_frame).c_str(), nullptr, filter_graph_), "create left buffer");
  ffmpeg_check(avfilter_graph_create_filter(&buffersrc_right_ctx_, buffersrc, "in_right", format_filter_args(right_frame).c_str(), nullptr, filter_graph_), "create right buffer");
  ffmpeg_check(avfilter_graph_create_filter(&buffersink_ctx_, buffersink, "out", nullptr, nullptr, filter_graph_), "create sink");

  std::string filter_description = build_filter_description(pane_width_, pane_height_, left_input_.colorspace, left_input_.range, right_input_.colorspace, right_input_.range, roi_enabled_local, roi_effective);

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
}

void ScopeWindow::ensure_texture() {
  if (texture_ == nullptr) {
    texture_ = static_cast<SDL_Texture*>(sdl_check_ptr(SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, pane_width_ * 2, pane_height_), "SDL_CreateTexture"));
  }
}

void ScopeWindow::present_frame(const AVFrame* filtered_frame) {
  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 100);
  sdl_check_bool(SDL_RenderClear(renderer_) == 0, "SDL_RenderClear");

  if (filtered_frame != nullptr) {
    sdl_check_bool(SDL_UpdateTexture(texture_, nullptr, filtered_frame->data[0], filtered_frame->linesize[0]) == 0, "SDL_UpdateTexture");
    sdl_check_bool(SDL_RenderCopy(renderer_, texture_, nullptr, nullptr) == 0, "SDL_RenderCopy");
  }

  draw_left_right_overlay(renderer_, window_width_, window_height_, window_width_ / 2);

  SDL_RenderPresent(renderer_);
}

bool ScopeWindow::prepare(const AVFrame* left_frame, const AVFrame* right_frame) {
  if (!left_frame || !right_frame) {
    return false;
  }

  bool roi_enabled_local = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    roi_enabled_local = roi_enabled_;

    if (graph_reset_pending_) {
      destroy_graph();
      graph_reset_pending_ = false;
    }
  }

  // If ROI is disabled, we still want to blank in render, but skip compute here.
  if (!roi_enabled_local) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (pending_frame_ != nullptr) {
      av_frame_free(&pending_frame_);
      pending_frame_ = nullptr;
    }
    return true;
  }

  ensure_graph(left_frame, right_frame);

  // Drain any pending frames to prevent lag and ensure one-to-one updates
  for (;;) {
    AVFrame* pending = av_frame_alloc();
    if (!pending) {
      throw std::runtime_error("av_frame_alloc failed (pending)");
    }
    const int pending_result = av_buffersink_get_frame(buffersink_ctx_, pending);
    av_frame_free(&pending);
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
    AVFrame* candidate = av_frame_alloc();
    if (!candidate) {
      throw std::runtime_error("av_frame_alloc failed");
    }
    const int receive_result = av_buffersink_get_frame(buffersink_ctx_, candidate);
    if (receive_result != 0) {
      av_frame_free(&candidate);
      break;
    }
    if (latest_frame != nullptr) {
      av_frame_free(&latest_frame);
    }
    latest_frame = candidate;
  }

  if (latest_frame != nullptr) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (pending_frame_ != nullptr) {
      av_frame_free(&pending_frame_);
      pending_frame_ = nullptr;
    }
    pending_frame_ = latest_frame;
    frame_counter_++;
    last_pts_left_ = left_frame->pts;
    last_pts_right_ = right_frame->pts;
    return true;
  }

  return false;
}

void ScopeWindow::render() {
  Roi roi_local;
  bool roi_enabled_local = false;
  int source_width = 0;
  int source_height = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    roi_local = roi_;
    roi_enabled_local = roi_enabled_;
    source_width = source_width_;
    source_height = source_height_;
  }

  // Update title (main thread only) when ROI is smaller than the full frame.
  std::string title = base_title_;
  if (roi_enabled_local && source_width > 0 && source_height > 0) {
    const Roi effective_roi = clamp_roi_to_frame(roi_local, source_width, source_height);
    const bool is_full = (effective_roi.w == source_width && effective_roi.h == source_height);
    if (!is_full) {
      title += string_sprintf("   (%d,%d)-(%d,%d)", effective_roi.x, effective_roi.y, effective_roi.x + effective_roi.w - 1, effective_roi.y + effective_roi.h - 1);
    }
  }
  if (title != last_window_title_) {
    SDL_SetWindowTitle(window_, title.c_str());
    last_window_title_ = title;
  }

  // Always clear/present to keep the window responsive.
  if (!roi_enabled_local) {
    ensure_texture();
    present_frame(nullptr);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (texture_reset_pending_) {
      if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
      }
      texture_reset_pending_ = false;
    }
  }

  ensure_texture();

  AVFrame* to_present = nullptr;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    to_present = pending_frame_;
    pending_frame_ = nullptr;
  }

  present_frame(to_present);

  if (to_present != nullptr) {
    av_frame_free(&to_present);
  }
}

bool ScopeWindow::update(const AVFrame* left_frame, const AVFrame* right_frame) {
  const bool prepared = prepare(left_frame, right_frame);
  render();
  return prepared;
}

bool ScopeWindow::handle_event(const SDL_Event& event) {
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

  // Only process events directed to this window
  if (event_window_id == 0 || event_window_id != window_id_) {
    return false;
  }

  switch (event.type) {
    case SDL_WINDOWEVENT:
      if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
        close_requested_ = true;
      } else if (event.window.event == SDL_WINDOWEVENT_RESIZED || event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        // Update internal sizing and reinitialize resources to match
        const int new_width = event.window.data1;
        const int new_height = event.window.data2;

        if (new_width > 0 && new_height > 0) {
          window_width_ = new_width;
          window_height_ = new_height;

          pane_width_ = std::max(1, window_width_ / 2);
          pane_height_ = std::max(1, window_height_);

          // Recreate resources on next cycle to avoid cross-thread SDL calls
          {
            std::lock_guard<std::mutex> lock(state_mutex_);
            texture_reset_pending_ = true;
            graph_reset_pending_ = true;
          }

          // Let main display poll this event and trigger refresh
          return false;
        }
      }
      return true;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEWHEEL:
      return true;
  }

  // For other events to this window that we do not explicitly handle, do not consume by default
  return false;
}

void ScopeWindow::set_roi(const Roi& roi) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  roi_ = roi;
  roi_enabled_ = roi.w > 0 && roi.h > 0;
  graph_reset_pending_ = true;
}

void ScopeWindow::route_events(std::array<std::unique_ptr<ScopeWindow>, ScopeWindow::kNumScopes>& windows) {
  std::vector<SDL_Event> deferred_events;

  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    bool consumed_this_event = false;
    for (auto& window : windows) {
      if (window && window->handle_event(event)) {
        consumed_this_event = true;

        // Defer actual destruction to the main loop to coordinate with worker threads
      }
    }

    if (!consumed_this_event) {
      deferred_events.push_back(event);
    }
  }

  // Requeue events for the main display input to process
  for (const auto& deferred_event : deferred_events) {
    SDL_Event event_copy;
    std::memset(&event_copy, 0, sizeof(event_copy));
    std::memcpy(&event_copy, &deferred_event, sizeof(SDL_Event));
    const int result = SDL_PushEvent(&event_copy);
    if (result != 1) {
      continue;
    }
  }
}