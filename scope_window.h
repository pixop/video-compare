#pragma once
#include <memory>
#include <string>
#include <limits.h>
#include <cstdint>
#include <array>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>
}
// Forward declarations to avoid leaking SDL headers outside this translation unit
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
union SDL_Event;

class ScopeWindow {
 public:
  enum class Type { Histogram, Vectorscope, Waveform };
  static constexpr size_t kNumScopes = 3;
  static constexpr size_t index(const Type t) {
    switch (t) {
      case Type::Histogram:
        return 0;
      case Type::Vectorscope:
        return 1;
      case Type::Waveform:
        return 2;
    }
    return 0;
  }
  static constexpr std::array<Type, kNumScopes> all_types() {
    return {Type::Histogram, Type::Vectorscope, Type::Waveform};
  }
  static constexpr Type type_for_index(const size_t idx) {
    switch (idx) {
      case 0:
        return Type::Histogram;
      case 1:
        return Type::Vectorscope;
      case 2:
        return Type::Waveform;
      default:
        return Type::Histogram;
    }
  }

  ScopeWindow(Type type, const int pane_width, const int pane_height, const bool always_on_top, const int display_number, const bool use_10_bpc);
  ~ScopeWindow();

  // Feed the current frames and update the scope window if an output is produced.
  bool update(const AVFrame* left_frame, const AVFrame* right_frame);

  // Accessor for this window's type
  Type get_type() const { return type_; }

  bool close_requested() const { return close_requested_; }

  // Visible region of interest (ROI) in video coordinates
  struct Roi {
    int x;
    int y;
    int w;
    int h;
  };
  void set_roi(const Roi& roi);

  // Global routing: consume scope-window events and request refresh if resizing occurred
  static void route_events(std::array<std::unique_ptr<ScopeWindow>, ScopeWindow::kNumScopes>& windows);

 private:
  // Handle SDL events targeted to this window; returns true if consumed.
  bool handle_event(const SDL_Event& event);

  void ensure_graph(const AVFrame* left_frame, const AVFrame* right_frame);
  void destroy_graph();

  std::string build_filter_description(const int pane_width,
                                       const int pane_height,
                                       const int left_colorspace,
                                       const int left_range,
                                       const int right_colorspace,
                                       const int right_range) const;
  static std::string format_filter_args(const AVFrame* frame);

  void present_frame(const AVFrame* filtered_frame);

 private:
  // SDL
  SDL_Window* window_{nullptr};
  SDL_Renderer* renderer_{nullptr};
  SDL_Texture* texture_{nullptr};
  int window_width_{0};
  int window_height_{0};
  uint32_t window_id_{0};
  bool close_requested_{false};

  // FFmpeg filter graph
  AVFilterGraph* filter_graph_{nullptr};
  AVFilterContext* buffersrc_left_ctx_{nullptr};
  AVFilterContext* buffersrc_right_ctx_{nullptr};
  AVFilterContext* buffersink_ctx_{nullptr};

  // Input tracking for reinitialization
  int input_width_left_{0};
  int input_height_left_{0};
  int input_format_left_{-1};
  int input_width_right_{0};
  int input_height_right_{0};
  int input_format_right_{-1};
  int input_colorspace_left_{0};
  int input_range_left_{0};
  int input_colorspace_right_{0};
  int input_range_right_{0};

  // Config
  Type type_;
  int pane_width_;
  int pane_height_;
  bool always_on_top_;
  int display_number_;
  bool use_10_bpc_;

  // PTS tracking to detect non-monotonic browsing and reset the graph
  int64_t last_pts_left_{INT64_MIN};
  int64_t last_pts_right_{INT64_MIN};
  int64_t frame_counter_{0};

  // ROI tracking
  bool roi_enabled_{false};
  Roi roi_{0, 0, 0, 0};
  Roi prev_roi_{-1, -1, -1, -1};
};


