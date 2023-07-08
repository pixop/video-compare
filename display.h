#pragma once
#include <SDL2/SDL_ttf.h>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include "SDL2/SDL.h"

struct SDL {
  SDL();
  ~SDL();
};

class Display {
 public:
  enum Mode { split, vstack, hstack };

 private:
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
  float window_to_drawable_width_factor_;
  float window_to_drawable_height_factor_;
  float screen_to_video_width_factor_;
  float screen_to_video_height_factor_;
  float font_scale_;

  bool quit_{false};
  bool play_{true};
  bool swap_left_right_{false};
  bool zoom_left_{false};
  bool zoom_right_{false};
  bool show_left_{true};
  bool show_right_{true};
  bool show_hud_{true};
  bool subtraction_mode_{false};
  float seek_relative_{0.0F};
  int frame_offset_delta_{0};
  int shift_right_frames_{0};
  bool seek_from_start_{false};
  bool save_image_frames_{false};
  bool print_mouse_position_and_color_{false};
  bool mouse_is_inside_window_{false};

  SDL sdl_;
  TTF_Font* small_font_;
  TTF_Font* big_font_;
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

  std::chrono::milliseconds error_message_shown_at_;
  SDL_Texture* error_message_texture_{nullptr};
  int error_message_width_;
  int error_message_height_;

  SDL_Window* window_;
  SDL_Renderer* renderer_;
  SDL_Texture* video_texture_;
  SDL_Texture* zoom_texture_;

  SDL_Event event_;
  int mouse_x_;
  int mouse_y_;

  const std::string left_file_stem_;
  const std::string right_file_stem_;
  int saved_image_number_{1};

  void convert_to_packed_10_bpc(std::array<uint8_t*, 3> in_planes, std::array<size_t, 3> in_pitches, std::array<uint32_t*, 3> out_planes, std::array<size_t, 3> out_pitches, const SDL_Rect& roi);

  void update_difference(std::array<uint8_t*, 3> planes_left, std::array<size_t, 3> pitches_left, std::array<uint8_t*, 3> planes_right, std::array<size_t, 3> pitches_right, int split_x);

  void save_image_frames(std::array<uint8_t*, 3> planes_left, std::array<size_t, 3> pitches_left, std::array<uint8_t*, 3> planes_right, std::array<size_t, 3> pitches_right);

  inline int static round(const float value) { return static_cast<int>(std::round(value)); }

  SDL_Rect video_to_screen_space(const SDL_Rect& rect) const {
    const float width_scale = (high_dpi_allowed_ ? 2.F : 1.F) / screen_to_video_width_factor_;
    const float height_scale = (high_dpi_allowed_ ? 2.F : 1.F) / screen_to_video_height_factor_;

    return {round(static_cast<float>(rect.x) * width_scale), round(static_cast<float>(rect.y) * height_scale), round(static_cast<float>(rect.w) * width_scale), round(static_cast<float>(rect.h) * height_scale)};
  }

  void render_text(int x, int y, SDL_Texture* texture, int texture_width, int texture_height, int border_extension, bool left_adjust);

  void update_textures(const SDL_Rect* rect, const void* pixels, int pitch, const std::string& error_message);

  int round_and_clamp(float value);

  const std::array<int, 3> get_rgb_pixel(uint8_t* rgb_plane, size_t pitch, int x, int y);
  const std::array<int, 3> convert_rgb_to_yuv(const std::array<int, 3> rgb);

  std::string format_pixel(const std::array<int, 3>& rgb);
  std::string get_and_format_rgb_yuv_pixel(uint8_t* rgb_plane, size_t pitch, int x, int y);

 public:
  Display(int display_number, Mode mode, bool high_dpi_allowed, bool use_10_bpc, std::tuple<int, int> window_size, unsigned width, unsigned height, double duration, const std::string& left_file_name, const std::string& right_file_name);
  ~Display();

  // Copy frame to display
  void refresh(std::array<uint8_t*, 3> planes_left,
               std::array<size_t, 3> pitches_left,
               std::array<uint8_t*, 3> planes_right,
               std::array<size_t, 3> pitches_right,
               float left_position,
               const std::string& left_picture_type,
               float right_position,
               const std::string& right_picture_type,
               const std::string& current_total_browsable,
               const std::string& error_message);

  // Handle events
  void input();

  bool get_quit() const;
  bool get_play() const;
  bool get_swap_left_right() const;
  float get_seek_relative() const;
  bool get_seek_from_start() const;
  int get_frame_offset_delta() const;
  int get_shift_right_frames() const;
};
