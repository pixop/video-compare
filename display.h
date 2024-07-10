#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
}

struct SDL {
  SDL();
  ~SDL();
};

class Vector2D {
 public:
  Vector2D(float px, float py) : x_(px), y_(py) {}

  float x() const { return x_; }
  float y() const { return y_; }

  Vector2D operator+(const Vector2D& v) const { return Vector2D(x_ + v.x_, y_ + v.y_); }

  Vector2D operator-(const Vector2D& v) const { return Vector2D(x_ - v.x_, y_ - v.y_); }

  Vector2D operator*(const Vector2D& v) const { return Vector2D(x_ * v.x_, y_ * v.y_); }

  Vector2D operator/(const Vector2D& v) const { return Vector2D(x_ / v.x_, y_ / v.y_); }

  Vector2D operator+(const float scalar) const { return Vector2D(x_ + scalar, y_ + scalar); }

  Vector2D operator-(const float scalar) const { return Vector2D(x_ - scalar, y_ - scalar); }

  Vector2D operator*(const float scalar) const { return Vector2D(x_ * scalar, y_ * scalar); }

  Vector2D operator/(const float scalar) const { return Vector2D(x_ / scalar, y_ / scalar); }

  friend std::ostream& operator<<(std::ostream& os, const Vector2D& v) { return os << "(" << v.x_ << ", " << v.y_ << ")"; }

 private:
  float x_, y_;
};

class Display {
 public:
  enum Mode { split, vstack, hstack };
  enum Loop { off, forwardonly, pingpong };

  std::string modeToString(const Mode& mode) {
    switch (mode) {
      case split:
        return "split";
      case vstack:
        return "vstack";
      case hstack:
        return "hstack";
      default:
        return "unknown";
    }
  }

 private:
  const int display_number_;
  const Mode mode_;
  const bool high_dpi_allowed_;
  const bool use_10_bpc_;
  const int video_width_;
  const int video_height_;
  const double duration_;

  int drawable_width_;
  int drawable_height_;
  int window_width_;
  int window_height_;
  float drawable_to_window_width_factor_;
  float drawable_to_window_height_factor_;
  float video_to_window_width_factor_;
  float video_to_window_height_factor_;
  float font_scale_;

  bool show_help_{false};
  bool quit_{false};
  bool play_{true};
  Loop buffer_play_loop_mode_{off};
  bool buffer_play_forward_{true};
  bool swap_left_right_{false};
  bool zoom_left_{false};
  bool zoom_right_{false};
  bool show_left_{true};
  bool show_right_{true};
  bool show_hud_{true};
  bool subtraction_mode_{false};
  float seek_relative_{0.0F};
  int frame_buffer_offset_delta_{0};
  int frame_navigation_delta_{0};
  int shift_right_frames_{0};
  bool seek_from_start_{false};
  bool save_image_frames_{false};
  bool print_mouse_position_and_color_{false};
  bool print_image_similarity_metrics_{false};
  bool mouse_is_inside_window_{false};
  int playback_speed_level_{0};
  float playback_speed_factor_{1.0F};
  bool tick_playback_{false};

  float global_zoom_level_{0.0F};
  float global_zoom_factor_{1.0F};
  Vector2D move_offset_{0.0F, 0.0F};
  Vector2D global_center_{0.5F, 0.5F};

  SDL sdl_;
  TTF_Font* small_font_;
  TTF_Font* big_font_;
  SDL_Cursor* normal_mode_cursor_;
  SDL_Cursor* pan_mode_cursor_;
  uint8_t* diff_buffer_;
  uint32_t* left_buffer_{nullptr};
  uint32_t* right_buffer_{nullptr};
  std::array<uint8_t*, 3> diff_planes_;
  std::array<uint32_t*, 3> left_planes_;
  std::array<uint32_t*, 3> right_planes_;
  std::array<size_t, 3> diff_pitches_;

  SDL_Texture* left_text_texture_;
  SDL_Texture* right_text_texture_;
  int left_text_width_;
  int left_text_height_;
  int right_text_width_;
  int right_text_height_;

  int border_extension_;
  int double_border_extension_;
  int line1_y_;
  int line2_y_;
  int middle_y_;
  int max_text_width_;

  std::chrono::milliseconds message_shown_at_;
  SDL_Texture* message_texture_{nullptr};
  int message_width_;
  int message_height_;

  SDL_Window* window_;
  SDL_Renderer* renderer_;
  SDL_Texture* video_texture_;

  SDL_Event event_;
  int mouse_x_;
  int mouse_y_;
  float wheel_sensitivity_;

  const std::string left_file_stem_;
  const std::string right_file_stem_;
  int saved_image_number_{1};

  std::vector<SDL_Texture*> help_textures_;
  int help_total_height_{0};
  int help_y_offset_{0};

  void print_verbose_info();

  void convert_to_packed_10_bpc(std::array<uint8_t*, 3> in_planes, std::array<size_t, 3> in_pitches, std::array<uint32_t*, 3> out_planes, std::array<size_t, 3> out_pitches, const SDL_Rect& roi);

  void update_difference(std::array<uint8_t*, 3> planes_left, std::array<size_t, 3> pitches_left, std::array<uint8_t*, 3> planes_right, std::array<size_t, 3> pitches_right, int split_x);

  void save_image_frames(const AVFrame* left_frame, const AVFrame* right_frame);

  inline int static round(const float value) { return static_cast<int>(std::round(value)); }

  SDL_FRect video_rect_to_drawable_transform(const SDL_FRect& rect) const {
    const float width_scale = drawable_to_window_width_factor_ / video_to_window_width_factor_;
    const float height_scale = drawable_to_window_height_factor_ / video_to_window_height_factor_;

    return {rect.x * width_scale, rect.y * height_scale, rect.w * width_scale, rect.h * height_scale};
  }

  void render_text(int x, int y, SDL_Texture* texture, int texture_width, int texture_height, int border_extension, bool left_adjust);

  void render_progress_dots(const float position, const float progress, const bool is_top);

  void update_textures(const SDL_Rect* rect, const void* pixels, int pitch, const std::string& message);

  int round_and_clamp(const float value);

  const std::array<int, 3> get_rgb_pixel(uint8_t* rgb_plane, const size_t pitch, const int x, const int y);
  const std::array<int, 3> convert_rgb_to_yuv(const std::array<int, 3> rgb);

  std::string format_pixel(const std::array<int, 3>& rgb);
  std::string get_and_format_rgb_yuv_pixel(uint8_t* rgb_plane, const size_t pitch, const int x, const int y);

  float* rgb_to_grayscale(const uint8_t* plane, const size_t pitch);

  float compute_ssim_block(const float* left_plane, const float* right_plane, const int x_offset, const int y_offset, const int block_size);
  float compute_ssim(const float* left_plane, const float* right_plane);

  float compute_psnr(const float* left_plane, const float* right_plane);

  void render_help();

  float compute_zoom_factor(const float zoom_level) const;
  Vector2D compute_relative_move_offset(const Vector2D& zoom_point, const float zoom_factor) const;
  void update_zoom_factor_and_move_offset(const float zoom_factor);
  void update_zoom_factor(const float zoom_factor);
  void update_move_offset(const Vector2D& move_offset);

  void update_playback_speed(const int playback_speed_level);

 public:
  Display(int display_number,
          Mode mode,
          bool verbose,
          bool high_dpi_allowed,
          bool use_10_bpc,
          std::tuple<int, int> window_size,
          unsigned width,
          unsigned height,
          double duration,
          const float wheel_sensitivity,
          const std::string& left_file_name,
          const std::string& right_file_name);
  ~Display();

  // Copy frame to display
  void refresh(const AVFrame* left_frame, const AVFrame* right_frame, const std::string& current_total_browsable, const std::string& message);

  // Handle events
  void input();

  bool get_quit() const;
  bool get_play() const;
  Loop get_buffer_play_loop_mode() const;
  void set_buffer_play_loop_mode(const Loop& mode);
  bool get_buffer_play_forward() const;
  void toggle_buffer_play_direction();
  bool get_swap_left_right() const;
  float get_seek_relative() const;
  bool get_seek_from_start() const;
  int get_frame_buffer_offset_delta() const;
  int get_frame_navigation_delta() const;
  int get_shift_right_frames() const;
  float get_playback_speed_factor() const;
  bool get_tick_playback() const;
};
