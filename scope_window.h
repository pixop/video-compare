#pragma once
#include <limits.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
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
  static const char* type_to_string(Type t);
  static size_t index(Type t);
  static Type type_for_index(size_t idx);
  static std::array<Type, kNumScopes> all_types();

  ScopeWindow(Type type, const int pane_width, const int pane_height, const bool always_on_top, const int display_number, const bool use_10_bpc, const std::string& filter_options);
  ~ScopeWindow();

  // Feed the current frames and update the scope window if an output is produced.
  bool update(const AVFrame* left_frame, const AVFrame* right_frame);
  // Compute-only phase: run FFmpeg filter graph and store the freshest frame for later rendering.
  bool prepare(const AVFrame* left_frame, const AVFrame* right_frame);
  // Render phase: must run on the main thread; uploads the pending frame (if any) and presents.
  void render();

  // Accessor for this window's type
  Type get_type() const { return type_; }

  bool close_requested() const { return close_requested_.load(); }
  void request_close() { close_requested_.store(true); }

  // Visible region of interest (ROI) in video coordinates
  struct Roi {
    int x;
    int y;
    int w;
    int h;
  };
  void set_roi(const Roi& roi);

  uint32_t window_id() const { return window_id_; }

  // Handle SDL events targeted to this window; returns true if consumed.
  bool handle_event(const SDL_Event& event);

 private:
  void ensure_graph(const AVFrame* left_frame, const AVFrame* right_frame);
  void ensure_texture();
  void destroy_graph();

  std::string build_filter_description(const int pane_width, const int pane_height, const int left_colorspace, const int left_range, const int right_colorspace, const int right_range, const bool roi_enabled, const Roi& roi) const;
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
  std::atomic<bool> close_requested_{false};

  // FFmpeg filter graph
  AVFilterGraph* filter_graph_{nullptr};
  AVFilterContext* buffersrc_left_ctx_{nullptr};
  AVFilterContext* buffersrc_right_ctx_{nullptr};
  AVFilterContext* buffersink_ctx_{nullptr};

  // Input tracking for reinitialization
  struct InputState {
    int width;
    int height;
    int format;
    int colorspace;
    int range;
  };
  InputState left_input_{0, 0, -1, 0, 0};
  InputState right_input_{0, 0, -1, 0, 0};

  // Config
  Type type_;
  int pane_width_;
  int pane_height_;
  bool always_on_top_;
  int display_number_;
  bool use_10_bpc_;
  std::string filter_options_;
  std::string base_title_;
  std::string last_window_title_;

  // PTS tracking to detect non-monotonic browsing and reset the graph
  int64_t last_pts_left_{INT64_MIN};
  int64_t last_pts_right_{INT64_MIN};
  int64_t frame_counter_{0};

  // ROI tracking
  bool roi_enabled_{false};
  Roi roi_{0, 0, 0, 0};
  Roi prev_roi_{-1, -1, -1, -1};
  int source_width_{0};
  int source_height_{0};

  // Cross-thread coordination
  std::mutex state_mutex_;
  AVFrame* pending_frame_{nullptr};  // Owned; freed on render
  bool texture_reset_pending_{false};
  bool graph_reset_pending_{false};
};
