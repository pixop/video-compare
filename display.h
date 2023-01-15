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

  SDL sdl_;
  TTF_Font* small_font_;
  TTF_Font* big_font_;
  uint8_t* diff_buffer_;
  std::array<uint8_t*, 3> diff_planes_;

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
  SDL_Texture* texture_;

  SDL_Event event_;
  int mouse_x_;
  int mouse_y_;

  const std::string left_file_stem_;
  const std::string right_file_stem_;
  int saved_image_number_{1};

  void update_difference(std::array<uint8_t*, 3> planes_left, std::array<size_t, 3> pitches_left, std::array<uint8_t*, 3> planes_right, std::array<size_t, 3> pitches_right, int split_x);

  void save_image_frames(std::array<uint8_t*, 3> planes_left, std::array<size_t, 3> pitches_left, std::array<uint8_t*, 3> planes_right, std::array<size_t, 3> pitches_right);

  inline int static round(const float value) { return static_cast<int>(std::round(value)); }

  SDL_Rect video_to_screen_space(const SDL_Rect& rect) const {
    const float width_scale = (high_dpi_allowed_ ? 2.F : 1.F) / screen_to_video_width_factor_;
    const float height_scale = (high_dpi_allowed_ ? 2.F : 1.F) / screen_to_video_height_factor_;

    return {round(static_cast<float>(rect.x) * width_scale), round(static_cast<float>(rect.y) * height_scale), round(static_cast<float>(rect.w) * width_scale), round(static_cast<float>(rect.h) * height_scale)};
  }

  void render_text(int x, int y, SDL_Texture* texture, int texture_width, int texture_height, int border_extension, bool left_adjust);

 public:
  Display(Mode mode, bool high_dpi_allowed, std::tuple<int, int> window_size, unsigned width, unsigned height, double duration, const std::string& left_file_name, const std::string& right_file_name);
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
