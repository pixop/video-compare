#include "display.h"
#include <libgen.h>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include "source_code_pro_regular_ttf.h"
#include "string_utils.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

template <typename T>
inline T check_sdl(T value, const std::string& message) {
  if (!value) {
    throw std::runtime_error{"SDL " + message + " - " + SDL_GetError()};
  }
  return value;
}

inline int clamp_int_to_byte_range(int value) {
  return value > 255 ? 255 : value < 0 ? 0 : value;
}

inline int clamp_int_to_10_bpc_range(int value) {
  return value > 1023 ? 1023 : value < 0 ? 0 : value;
}

inline uint8_t clamp_int_to_byte(int value) {
  return static_cast<uint8_t>(clamp_int_to_byte_range(value));
}

inline uint16_t clamp_int_to_10_bpc(int value) {
  return static_cast<uint16_t>(clamp_int_to_10_bpc_range(value));
}

// Credits to Kemin Zhou for this approach which does not require Boost or C++17
// https://stackoverflow.com/questions/4430780/how-can-i-extract-the-file-name-and-extension-from-a-path-in-c
std::string get_file_stem(const std::string& filePath) {
  char* buff = new char[filePath.size() + 1];
  strcpy(buff, filePath.c_str());

  std::string tmp = std::string(basename(buff));

  const std::string::size_type i = tmp.rfind('.');

  if (i != std::string::npos) {
    tmp = tmp.substr(0, i);
  }

  delete[] buff;

  return tmp;
}

static const SDL_Color TEXT_COLOR = {255, 255, 255, 0};
static const SDL_Color POSITION_COLOR = {255, 255, 192, 0};
static const SDL_Color TARGET_COLOR = {200, 200, 140, 0};
static const SDL_Color BUFFER_COLOR = {160, 225, 192, 0};
static const int BACKGROUND_ALPHA = 100;

static std::string format_position(const float position) {
  const float rounded_millis = std::round(position * 1000.0F);

  const int milliseconds = rounded_millis;
  const int seconds = milliseconds / 1000;
  const int minutes = seconds / 60;
  const int hours = minutes / 60;

  if (minutes >= 60) {
    return string_sprintf("%02d:%02d:%02d.%03d", hours, minutes % 60, seconds % 60, milliseconds % 1000);
  }
  if (seconds >= 60) {
    return string_sprintf("%02d:%02d.%03d", minutes, seconds % 60, milliseconds % 1000);
  }

  return string_sprintf("%d.%03d", seconds, milliseconds % 1000);
}

static std::string format_position_difference(const float position1, const float position2) {
  if (std::abs(position1 - position2) <= 1e-4) {
    return "";
  } else if (position1 < position2) {
    return " (-" + format_position(position2 - position1) + ")";
  }

  return " (+" + format_position(position1 - position2) + ")";
}

SDL::SDL() {
  check_sdl(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0, "SDL init");
  check_sdl(TTF_Init() == 0, "TTF init");

  SDL_version linked;
  SDL_GetVersion(&linked);
  std::cout << "SDL version: " << string_sprintf("%u.%u.%u", linked.major, linked.minor, linked.patch) << std::endl;
}

SDL::~SDL() {
  SDL_Quit();
}

Display::Display(const int display_number,
                 const Mode mode,
                 const bool high_dpi_allowed,
                 const bool use_10_bpc,
                 const std::tuple<int, int> window_size,
                 const unsigned width,
                 const unsigned height,
                 const double duration,
                 const std::string& left_file_name,
                 const std::string& right_file_name)
    : mode_{mode},
      high_dpi_allowed_{high_dpi_allowed},
      use_10_bpc_{use_10_bpc},
      video_width_{static_cast<int>(width)},
      video_height_{static_cast<int>(height)},
      duration_{duration},
      left_file_stem_{get_file_stem(left_file_name)},
      right_file_stem_{get_file_stem(right_file_name)} {
  const int auto_width = mode == Mode::hstack ? width * 2 : width;
  const int auto_height = mode == Mode::vstack ? height * 2 : height;

  int window_width;
  int window_height;

  if (std::get<0>(window_size) < 0 && std::get<1>(window_size) < 0) {
    window_width = auto_width;
    window_height = auto_height;
  } else {
    if (std::get<0>(window_size) < 0) {
      window_height = std::get<1>(window_size);
      window_width = static_cast<float>(auto_width) / static_cast<float>(auto_height) * window_height;
    } else if (std::get<1>(window_size) < 0) {
      window_width = std::get<0>(window_size);
      window_height = static_cast<float>(auto_height) / static_cast<float>(auto_width) * window_width;
    } else {
      window_width = std::get<0>(window_size);
      window_height = std::get<1>(window_size);
    }
  }

  const int create_window_flags = SDL_WINDOW_SHOWN;

  window_ = check_sdl(SDL_CreateWindow("video-compare", SDL_WINDOWPOS_UNDEFINED_DISPLAY(display_number), SDL_WINDOWPOS_UNDEFINED_DISPLAY(display_number), high_dpi_allowed_ ? window_width / 2 : window_width,
                                       high_dpi_allowed_ ? window_height / 2 : window_height, high_dpi_allowed_ ? create_window_flags | SDL_WINDOW_ALLOW_HIGHDPI : create_window_flags),
                      "window");

  renderer_ = check_sdl(SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC), "renderer");

  SDL_RendererInfo info;
  SDL_GetRendererInfo(renderer_, &info);
  std::cout << "SDL renderer: " << info.name << std::endl;

  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
  SDL_RenderClear(renderer_);
  SDL_RenderPresent(renderer_);

  SDL_GL_GetDrawableSize(window_, &drawable_width_, &drawable_height_);
  SDL_GetWindowSize(window_, &window_width_, &window_height_);

  window_to_drawable_width_factor_ = static_cast<float>(drawable_width_) / static_cast<float>(window_width_);
  window_to_drawable_height_factor_ = static_cast<float>(drawable_height_) / static_cast<float>(window_height_);
  screen_to_video_width_factor_ = static_cast<float>(video_width_) / static_cast<float>(window_width_) * ((mode_ == Mode::hstack) ? 2.F : 1.F);
  screen_to_video_height_factor_ = static_cast<float>(video_height_) / static_cast<float>(window_height_) * ((mode_ == Mode::vstack) ? 2.F : 1.F);

  font_scale_ = (window_to_drawable_width_factor_ + window_to_drawable_height_factor_) / 2.0F;

  border_extension_ = 3 * font_scale_;
  double_border_extension_ = border_extension_ * 2;
  line1_y_ = 20;
  line2_y_ = line1_y_ + 30 * font_scale_;
  middle_y_ = drawable_height_ / 2 - (30 * font_scale_ - double_border_extension_) / 2;

  if (mode_ != Mode::vstack) {
    max_text_width_ = drawable_width_ / 2 - double_border_extension_ - line1_y_;
  } else {
    max_text_width_ = drawable_width_ - double_border_extension_ - line1_y_;
  }

  SDL_RWops* embedded_font = SDL_RWFromConstMem(SOURCE_CODE_PRO_REGULAR_TTF, SOURCE_CODE_PRO_REGULAR_TTF_LEN);
  small_font_ = check_sdl(TTF_OpenFontRW(embedded_font, 0, 16 * font_scale_), "font open");
  big_font_ = check_sdl(TTF_OpenFontRW(embedded_font, 0, 24 * font_scale_), "font open");

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
  SDL_RenderSetLogicalSize(renderer_, drawable_width_, drawable_height_);
  video_texture_ = check_sdl(SDL_CreateTexture(renderer_, use_10_bpc ? SDL_PIXELFORMAT_ARGB2101010 : SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, mode == Mode::hstack ? width * 2 : width, mode == Mode::vstack ? height * 2 : height),
                             "video texture");

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
  zoom_texture_ = check_sdl(SDL_CreateTexture(renderer_, use_10_bpc ? SDL_PIXELFORMAT_ARGB2101010 : SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, mode == Mode::hstack ? width * 2 : width, mode == Mode::vstack ? height * 2 : height),
                            "zoom texture");

  SDL_Surface* text_surface = TTF_RenderUTF8_Blended(small_font_, left_file_name.c_str(), TEXT_COLOR);
  left_text_texture_ = SDL_CreateTextureFromSurface(renderer_, text_surface);
  left_text_width_ = text_surface->w;
  left_text_height_ = text_surface->h;
  SDL_FreeSurface(text_surface);

  text_surface = TTF_RenderUTF8_Blended(small_font_, right_file_name.c_str(), TEXT_COLOR);
  right_text_texture_ = SDL_CreateTextureFromSurface(renderer_, text_surface);
  right_text_width_ = text_surface->w;
  right_text_height_ = text_surface->h;
  SDL_FreeSurface(text_surface);

  diff_buffer_ = new uint8_t[video_width_ * video_height_ * 3 * (use_10_bpc ? sizeof(uint16_t) : sizeof(uint8_t))];
  uint8_t* diff_plane_0 = diff_buffer_;

  diff_planes_ = {diff_plane_0, nullptr, nullptr};
  diff_pitches_ = {video_width_ * 3 * (use_10_bpc ? sizeof(uint16_t) : sizeof(uint8_t)), 0, 0};
}

Display::~Display() {
  SDL_DestroyTexture(video_texture_);
  SDL_DestroyTexture(zoom_texture_);
  SDL_DestroyTexture(left_text_texture_);
  SDL_DestroyTexture(right_text_texture_);

  if (error_message_texture_ != nullptr) {
    SDL_DestroyTexture(error_message_texture_);
  }

  TTF_CloseFont(small_font_);
  TTF_CloseFont(big_font_);

  delete[] diff_buffer_;

  if (left_buffer_ != nullptr) {
    delete[] left_buffer_;
  }
  if (right_buffer_ != nullptr) {
    delete[] right_buffer_;
  }

  SDL_DestroyRenderer(renderer_);
  SDL_DestroyWindow(window_);
}

void Display::convert_to_packed_10_bpc(std::array<uint8_t*, 3> in_planes, std::array<size_t, 3> in_pitches, std::array<uint32_t*, 3> out_planes, std::array<size_t, 3> out_pitches, const SDL_Rect& roi) {
  uint16_t* p_in = reinterpret_cast<uint16_t*>(in_planes[0] + roi.x * 6 + in_pitches[0] * roi.y);
  uint32_t* p_out = out_planes[0] + roi.x + out_pitches[0] * roi.y / 4;

  for (int y = 0; y < roi.h; y++) {
    for (int in_x = 0, out_x = 0; out_x < roi.w; in_x += 3, out_x++) {
      uint32_t r = p_in[in_x] >> 6;
      uint32_t g = p_in[in_x + 1] >> 6;
      uint32_t b = p_in[in_x + 2] >> 6;

      p_out[out_x] = (r << 20) | (g << 10) | (b);
    }

    p_in += in_pitches[0] / 2;
    p_out += out_pitches[0] / 4;
  }
}

void Display::update_difference(std::array<uint8_t*, 3> planes_left, std::array<size_t, 3> pitches_left, std::array<uint8_t*, 3> planes_right, std::array<size_t, 3> pitches_right, int split_x) {
  const int amplification = 2;

  if (use_10_bpc_) {
    uint16_t* p_left = reinterpret_cast<uint16_t*>(planes_left[0] + split_x * 6);
    uint16_t* p_right = reinterpret_cast<uint16_t*>(planes_right[0] + split_x * 6);
    uint16_t* p_diff = reinterpret_cast<uint16_t*>(diff_planes_[0] + split_x * 6);

    for (int y = 0; y < video_height_; y++) {
      for (int in_x = 0, out_x = 0; out_x < (video_width_ - split_x) * 3; in_x += 3, out_x += 3) {
        int rl = p_left[in_x] >> 6;
        int gl = p_left[in_x + 1] >> 6;
        int bl = p_left[in_x + 2] >> 6;

        int rr = p_right[in_x] >> 6;
        int gr = p_right[in_x + 1] >> 6;
        int br = p_right[in_x + 2] >> 6;

        int r_diff = abs(rl - rr) * amplification;
        int g_diff = abs(gl - gr) * amplification;
        int b_diff = abs(bl - br) * amplification;

        p_diff[out_x] = clamp_int_to_10_bpc(r_diff) << 6;
        p_diff[out_x + 1] = clamp_int_to_10_bpc(g_diff) << 6;
        p_diff[out_x + 2] = clamp_int_to_10_bpc(b_diff) << 6;
      }

      p_left += pitches_left[0] / 2;
      p_right += pitches_right[0] / 2;
      p_diff += video_width_ * 3;
    }
  } else {
    uint8_t* p_left = planes_left[0] + split_x * 3;
    uint8_t* p_right = planes_right[0] + split_x * 3;
    uint8_t* p_diff = diff_planes_[0] + split_x * 3;

    for (int y = 0; y < video_height_; y++) {
      for (int in_x = 0, out_x = 0; out_x < (video_width_ - split_x) * 3; in_x += 3, out_x += 3) {
        int rl = p_left[in_x];
        int gl = p_left[in_x + 1];
        int bl = p_left[in_x + 2];

        int rr = p_right[in_x];
        int gr = p_right[in_x + 1];
        int br = p_right[in_x + 2];

        int r_diff = abs(rl - rr) * amplification;
        int g_diff = abs(gl - gr) * amplification;
        int b_diff = abs(bl - br) * amplification;

        p_diff[out_x] = clamp_int_to_byte(r_diff);
        p_diff[out_x + 1] = clamp_int_to_byte(g_diff);
        p_diff[out_x + 2] = clamp_int_to_byte(b_diff);
      }

      p_left += pitches_left[0];
      p_right += pitches_right[0];
      p_diff += video_width_ * 3;
    }
  }
}

void Display::save_image_frames(std::array<uint8_t*, 3> planes_left, std::array<size_t, 3> pitches_left, std::array<uint8_t*, 3> planes_right, std::array<size_t, 3> pitches_right) {
  auto write_png = [this](std::array<uint8_t*, 3> planes, std::array<size_t, 3> pitches, const std::string& filename) {
    if (use_10_bpc_) {
      // for 10 bpc: create truncated 8 bpc version of 16 bpc input until stb supports 16-bit PNGs
      uint8_t* temp_image = new uint8_t[video_width_ * video_height_ * 3];
      uint8_t* p_out = temp_image;

      for (int y = 0; y < video_height_; y++) {
        uint8_t* p_in = planes[0] + y * pitches[0] + 1;

        for (int x = 0; x < (video_width_ * 3); x++, p_out++, p_in += 2) {
          *p_out = *p_in;
        }
      }

      if (stbi_write_png(filename.c_str(), video_width_, video_height_, 3, temp_image, video_width_ * 3) == 0) {
        std::cerr << "Error saving video PNG image to file: " << filename << std::endl;
      }

      delete[] temp_image;
    } else {
      if (stbi_write_png(filename.c_str(), video_width_, video_height_, 3, planes[0], pitches[0]) == 0) {
        std::cerr << "Error saving video PNG image to file: " << filename << std::endl;
      }
    }
  };

  const std::string left_filename = string_sprintf("%s_%04d.png", left_file_stem_.c_str(), saved_image_number_);
  const std::string right_filename = string_sprintf("%s_%04d.png", right_file_stem_.c_str(), saved_image_number_);

  std::thread save_left_frame_thread(write_png, planes_left, pitches_left, left_filename);
  std::thread save_right_frame_thread(write_png, planes_right, pitches_right, right_filename);

  save_left_frame_thread.join();
  save_right_frame_thread.join();

  std::cout << "Saved " << left_filename << " and " << right_filename << std::endl;

  if (use_10_bpc_) {
    std::cout << "Warning: 8-bit PNG format used due to lack of 16-bit PNG support in stb (noticable banding is expected due to loss of precision)" << std::endl;
  }

  saved_image_number_++;
}

void Display::render_text(const int x, const int y, SDL_Texture* texture, const int texture_width, const int texture_height, const int border_extension, const bool left_adjust) {
  // compute clip amount which ensures the filename does not extend more than half the display width
  const int clip_amount = std::max((texture_width + double_border_extension_) - max_text_width_, 0);
  const int gradient_amount = std::min(clip_amount, 24);

  SDL_Rect fill_rect = {x - border_extension + gradient_amount, y - border_extension, texture_width + double_border_extension_ - clip_amount - gradient_amount, texture_height + double_border_extension_};

  SDL_Rect src_rect = {clip_amount + gradient_amount, 0, texture_width - clip_amount - gradient_amount, texture_height};
  SDL_Rect text_rect = {x + gradient_amount, y, texture_width - clip_amount - gradient_amount, texture_height};

  if (!left_adjust && (mode_ != Mode::vstack)) {
    fill_rect.x += clip_amount;
    text_rect.x += clip_amount;
  }

  SDL_RenderFillRect(renderer_, &fill_rect);
  SDL_RenderCopy(renderer_, texture, &src_rect, &text_rect);

  // render gradient
  if (gradient_amount > 0) {
    Uint8 draw_color_r;
    Uint8 draw_color_g;
    Uint8 draw_color_b;
    Uint8 draw_color_a;
    Uint8 alpha_mod;

    SDL_GetRenderDrawColor(renderer_, &draw_color_r, &draw_color_g, &draw_color_b, &draw_color_a);
    SDL_GetTextureAlphaMod(texture, &alpha_mod);

    fill_rect.x--;
    fill_rect.w = 1;

    src_rect.x--;
    src_rect.w = 1;
    text_rect.x--;
    text_rect.w = 1;

    for (int i = (gradient_amount - 1); i >= 0; i--, fill_rect.x--, src_rect.x--, text_rect.x--) {
      SDL_SetRenderDrawColor(renderer_, draw_color_r, draw_color_g, draw_color_b, draw_color_a * i / gradient_amount);
      SDL_RenderFillRect(renderer_, &fill_rect);

      SDL_SetTextureAlphaMod(texture, alpha_mod * i / gradient_amount);
      SDL_RenderCopy(renderer_, texture, &src_rect, &text_rect);
    }

    // reset
    SDL_SetRenderDrawColor(renderer_, draw_color_r, draw_color_g, draw_color_b, draw_color_a);
    SDL_SetTextureAlphaMod(texture, alpha_mod);
  }
}

void Display::update_textures(const SDL_Rect* rect, const void* pixels, int pitch, const std::string& error_message) {
  check_sdl(SDL_UpdateTexture(video_texture_, rect, pixels, pitch) == 0, "video texture - " + error_message);

  if (zoom_left_ || zoom_right_) {
    // perform a full update - optimize later...
    check_sdl(SDL_UpdateTexture(zoom_texture_, rect, pixels, pitch) == 0, "zoom texture - " + error_message);
  }
}

int Display::round_and_clamp(float value) {
  int result = static_cast<int>(std::roundf(value));

  return use_10_bpc_ ? clamp_int_to_10_bpc_range(result) : clamp_int_to_byte_range(result);
}

const std::array<int, 3> Display::get_rgb_pixel(uint8_t* rgb_plane, size_t pitch, int x, int y) {
  int r, g, b;

  if (use_10_bpc_) {
    uint16_t* rgb_pixel = reinterpret_cast<uint16_t*>(rgb_plane + x * 6 + y * pitch);

    r = *(rgb_pixel) >> 6;
    g = *(rgb_pixel + 1) >> 6;
    b = *(rgb_pixel + 2) >> 6;

  } else {
    uint8_t* rgb_pixel = rgb_plane + x * 3 + y * pitch;

    r = *(rgb_pixel);
    g = *(rgb_pixel + 1);
    b = *(rgb_pixel + 2);
  }

  return {r, g, b};
}

const std::array<int, 3> Display::convert_rgb_to_yuv(const std::array<int, 3> rgb) {
  float scale = use_10_bpc_ ? 4.f : 1.f;

  float r = rgb[0] / (256.f * scale);
  float g = rgb[1] / (256.f * scale);
  float b = rgb[2] / (256.f * scale);

  // https://en.wikipedia.org/wiki/YCbCr#ITU-R_BT.601_conversion
  float y = scale * (16.f + 65.738f * r + 129.057f * g + 25.064f * b);
  float cr = scale * (128.f - 37.945f * r - 74.494f * g + 112.439f * b);
  float cb = scale * (128.f + 112.439f * r - 94.154f * g - 18.285f * b);

  return {round_and_clamp(y), round_and_clamp(cr), round_and_clamp(cb)};
}

std::string to_hex(uint32_t value, int width) {
  std::stringstream sstream;
  sstream << std::setfill('0') << std::setw(width) << std::hex << value;

  return sstream.str();
}

std::string Display::format_pixel(const std::array<int, 3>& pixel) {
  std::string hex_pixel = use_10_bpc_ ? to_hex((pixel[0] << 20) | (pixel[1] << 10) | pixel[2], 8) : to_hex((pixel[0] << 16) | (pixel[1] << 8) | pixel[2], 6);

  return use_10_bpc_ ? string_sprintf("(%4d,%4d,%4d#%s)", pixel[0], pixel[1], pixel[2], hex_pixel.c_str()) : string_sprintf("(%3d,%3d,%3d#%s)", pixel[0], pixel[1], pixel[2], hex_pixel.c_str());
}

std::string Display::get_and_format_rgb_yuv_pixel(uint8_t* rgb_plane, size_t pitch, int x, int y) {
  const std::array<int, 3> rgb = get_rgb_pixel(rgb_plane, pitch, x, y);
  const std::array<int, 3> yuv = convert_rgb_to_yuv(rgb);

  return "RGB" + format_pixel(rgb) + ", YUV" + format_pixel(yuv);
}

void Display::refresh(std::array<uint8_t*, 3> planes_left,
                      std::array<size_t, 3> pitches_left,
                      std::array<uint8_t*, 3> planes_right,
                      std::array<size_t, 3> pitches_right,
                      const float left_position,
                      const std::string& left_picture_type,
                      const float right_position,
                      const std::string& right_picture_type,
                      const std::string& current_total_browsable,
                      const std::string& error_message) {
  if (save_image_frames_) {
    save_image_frames(planes_left, pitches_left, planes_right, pitches_right);
    save_image_frames_ = false;
  }

  // init 10 bpc temp buffers
  if (use_10_bpc_) {
    if (left_buffer_ == nullptr) {
      left_buffer_ = new uint32_t[pitches_left[0] * video_height_ / 4];
      left_planes_ = {left_buffer_, nullptr, nullptr};
    }
    if (right_buffer_ == nullptr) {
      right_buffer_ = new uint32_t[pitches_right[0] * video_height_ / 4];
      right_planes_ = {right_buffer_, nullptr, nullptr};
    }
  }

  bool compare_mode = show_left_ && show_right_;

  int mouse_video_x = std::round(static_cast<float>(mouse_x_) * screen_to_video_width_factor_);
  int mouse_video_y = std::round(static_cast<float>(mouse_y_) * screen_to_video_height_factor_);

  // print pixel position and RGB color value
  if (print_mouse_position_and_color_ && mouse_is_inside_window_) {
    std::cout << string_sprintf("[%4d,%4d]", mouse_video_x, mouse_video_y);
    std::cout << " - Left: " << get_and_format_rgb_yuv_pixel(planes_left[0], pitches_left[0], mouse_video_x, mouse_video_y);
    std::cout << " - Right: " << get_and_format_rgb_yuv_pixel(planes_right[0], pitches_right[0], mouse_video_x, mouse_video_y);
    std::cout << std::endl;

    print_mouse_position_and_color_ = false;
  }

  // clear everything
  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
  SDL_RenderClear(renderer_);

  if (show_left_ || show_right_) {
    int split_x = (compare_mode && mode_ == Mode::split) ? mouse_video_x : show_left_ ? video_width_ : 0;

    // update video
    if (show_left_ && (split_x > 0)) {
      SDL_Rect tex_render_quad_left = {0, 0, split_x, video_height_};
      SDL_Rect screen_render_quad_left = video_to_screen_space(tex_render_quad_left);

      if (use_10_bpc_) {
        convert_to_packed_10_bpc(planes_left, pitches_left, left_planes_, pitches_left, tex_render_quad_left);

        update_textures(&tex_render_quad_left, left_planes_[0], pitches_left[0], "left update (10 bpc, video mode)");
      } else {
        update_textures(&tex_render_quad_left, planes_left[0], pitches_left[0], "left update (video mode)");
      }

      check_sdl(SDL_RenderCopy(renderer_, video_texture_, &tex_render_quad_left, &screen_render_quad_left) == 0, "left video texture render copy");
    }
    if (show_right_ && ((split_x < (video_width_ - 1)) || mode_ != Mode::split)) {
      int start_right = (mode_ == Mode::split) ? split_x : 0;
      int right_x_offset = (mode_ == Mode::hstack) ? video_width_ : 0;
      int right_y_offset = (mode_ == Mode::vstack) ? video_height_ : 0;

      SDL_Rect tex_render_quad_right = {right_x_offset + start_right, right_y_offset, (video_width_ - start_right), video_height_};
      SDL_Rect roi = {start_right, 0, (video_width_ - start_right), video_height_};
      SDL_Rect screen_render_quad_right = video_to_screen_space(tex_render_quad_right);

      if (subtraction_mode_) {
        update_difference(planes_left, pitches_left, planes_right, pitches_right, start_right);

        if (use_10_bpc_) {
          convert_to_packed_10_bpc(diff_planes_, diff_pitches_, right_planes_, pitches_right, roi);

          update_textures(&tex_render_quad_right, right_planes_[0] + start_right, pitches_right[0], "right update (10 bpc, subtraction mode)");
        } else {
          update_textures(&tex_render_quad_right, diff_planes_[0] + start_right * 3, video_width_ * 3, "right update (subtraction mode)");
        }
      } else {
        if (use_10_bpc_) {
          convert_to_packed_10_bpc(planes_right, pitches_right, right_planes_, pitches_right, roi);

          update_textures(&tex_render_quad_right, right_planes_[0] + start_right, pitches_right[0], "right update (10 bpc, video mode)");
        } else {
          update_textures(&tex_render_quad_right, planes_right[0] + start_right * 3, pitches_right[0], "right update (video mode)");
        }
      }

      check_sdl(SDL_RenderCopy(renderer_, video_texture_, &tex_render_quad_right, &screen_render_quad_right) == 0, "right video texture render copy");
    }
  }

  // zoomed area
  int src_zoomed_size = 64;
  int src_half_zoomed_size = src_zoomed_size / 2;
  int dst_zoomed_size = std::min(drawable_width_, drawable_height_) * 0.5F;
  int dst_half_zoomed_size = dst_zoomed_size / 2;

  if (zoom_left_ || zoom_right_) {
    SDL_Rect src_zoomed_area = {std::min(std::max(0, mouse_video_x - src_half_zoomed_size), video_width_ * ((mode_ == Mode::hstack) ? 2 : 1)),
                                std::min(std::max(0, mouse_video_y - src_half_zoomed_size), video_height_ * ((mode_ == Mode::vstack) ? 2 : 1)), src_zoomed_size, src_zoomed_size};

    if (zoom_left_) {
      SDL_Rect dst_zoomed_area = {0, drawable_height_ - dst_zoomed_size, dst_zoomed_size, dst_zoomed_size};
      SDL_RenderCopy(renderer_, zoom_texture_, &src_zoomed_area, &dst_zoomed_area);
    }
    if (zoom_right_) {
      SDL_Rect dst_zoomed_area = {drawable_width_ - dst_zoomed_size, drawable_height_ - dst_zoomed_size, dst_zoomed_size, dst_zoomed_size};
      SDL_RenderCopy(renderer_, zoom_texture_, &src_zoomed_area, &dst_zoomed_area);
    }
  }

  SDL_Rect fill_rect;
  SDL_Rect text_rect;
  SDL_Surface* text_surface;

  if (show_hud_) {
    // render background rectangles and text on top
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    if (show_left_) {
      // file name and current position of left video
      const std::string left_pos_str = format_position(left_position) + " " + left_picture_type + format_position_difference(left_position, right_position);
      text_surface = TTF_RenderText_Blended(small_font_, left_pos_str.c_str(), POSITION_COLOR);
      SDL_Texture* left_position_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
      int left_position_text_width = text_surface->w;
      int left_position_text_height = text_surface->h;
      SDL_FreeSurface(text_surface);

      render_text(line1_y_, line1_y_, left_text_texture_, left_text_width_, left_text_height_, border_extension_, true);
      render_text(line1_y_, line2_y_, left_position_text_texture, left_position_text_width, left_position_text_height, border_extension_, true);

      SDL_DestroyTexture(left_position_text_texture);
    }
    if (show_right_) {
      // file name and current position of right video
      const std::string right_pos_str = format_position(right_position) + " " + right_picture_type + format_position_difference(right_position, left_position);
      text_surface = TTF_RenderText_Blended(small_font_, right_pos_str.c_str(), POSITION_COLOR);
      SDL_Texture* right_position_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
      int right_position_text_width = text_surface->w;
      int right_position_text_height = text_surface->h;
      SDL_FreeSurface(text_surface);

      int text1_x;
      int text1_y;
      int text2_x;
      int text2_y;

      if (mode_ == Mode::vstack) {
        text1_x = line1_y_;
        text1_y = drawable_height_ - line2_y_ - right_text_height_;
        text2_x = line1_y_;
        text2_y = drawable_height_ - line1_y_ - right_text_height_;
      } else {
        text1_x = drawable_width_ - line1_y_ - right_text_width_;
        text1_y = line1_y_;
        text2_x = drawable_width_ - line1_y_ - right_position_text_width;
        text2_y = line2_y_;
      }

      render_text(text1_x, text1_y, right_text_texture_, right_text_width_, right_text_height_, border_extension_, false);
      render_text(text2_x, text2_y, right_position_text_texture, right_position_text_width, right_position_text_height, border_extension_, false);

      SDL_DestroyTexture(right_position_text_texture);
    }
    if (mouse_is_inside_window_) {
      // target seek position
      double target_position = static_cast<float>(mouse_x_) / static_cast<float>(window_width_) * duration_;

      const std::string target_pos_str = format_position(target_position);
      text_surface = TTF_RenderText_Blended(small_font_, target_pos_str.c_str(), TARGET_COLOR);
      SDL_Texture* target_position_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
      int target_position_text_width = text_surface->w;
      int target_position_text_height = text_surface->h;
      SDL_FreeSurface(text_surface);

      SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA * 2);
      render_text(drawable_width_ - line1_y_ - target_position_text_width, drawable_height_ - line1_y_ - target_position_text_height, target_position_text_texture, target_position_text_width, target_position_text_height, border_extension_,
                  false);

      SDL_DestroyTexture(target_position_text_texture);
    }

    // current frame / number of frames in history buffer
    text_surface = TTF_RenderText_Blended(small_font_, current_total_browsable.c_str(), BUFFER_COLOR);
    SDL_Texture* current_total_browsable_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
    int current_total_browsable_text_width = text_surface->w;
    int current_total_browsable_text_height = text_surface->h;
    SDL_FreeSurface(text_surface);

    int text_y = (mode_ == Mode::vstack) ? middle_y_ : line2_y_;

    fill_rect = {drawable_width_ / 2 - current_total_browsable_text_width / 2 - border_extension_, text_y - border_extension_, current_total_browsable_text_width + double_border_extension_,
                 current_total_browsable_text_height + double_border_extension_};
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA);
    SDL_RenderFillRect(renderer_, &fill_rect);
    text_rect = {drawable_width_ / 2 - current_total_browsable_text_width / 2, text_y, current_total_browsable_text_width, current_total_browsable_text_height};
    SDL_RenderCopy(renderer_, current_total_browsable_text_texture, nullptr, &text_rect);
    SDL_DestroyTexture(current_total_browsable_text_texture);
  }

  // render (optional) error message
  if (!error_message.empty()) {
    error_message_shown_at_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    text_surface = TTF_RenderText_Blended(big_font_, error_message.c_str(), TEXT_COLOR);
    error_message_texture_ = SDL_CreateTextureFromSurface(renderer_, text_surface);
    error_message_width_ = text_surface->w;
    error_message_height_ = text_surface->h;
    SDL_FreeSurface(text_surface);
  }
  if (error_message_texture_ != nullptr) {
    std::chrono::milliseconds now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    float keep_alpha = std::max(sqrtf(1.0F - (now - error_message_shown_at_).count() / 1000.0F / 4.0F), 0.0F);

    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA * keep_alpha);
    fill_rect = {drawable_width_ / 2 - error_message_width_ / 2 - 2, drawable_height_ / 2 - error_message_height_ / 2 - 2, error_message_width_ + 4, error_message_height_ + 4};
    SDL_RenderFillRect(renderer_, &fill_rect);

    SDL_SetTextureAlphaMod(error_message_texture_, 255 * keep_alpha);
    text_rect = {drawable_width_ / 2 - error_message_width_ / 2, drawable_height_ / 2 - error_message_height_ / 2, error_message_width_, error_message_height_};
    SDL_RenderCopy(renderer_, error_message_texture_, nullptr, &text_rect);
  }

  if (mode_ == Mode::split && show_hud_ && compare_mode) {
    int draw_x = std::round(static_cast<float>(mouse_x_) * window_to_drawable_width_factor_);

    // render movable slider(s)
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, SDL_ALPHA_OPAQUE);
    SDL_RenderDrawLine(renderer_, draw_x, 0, draw_x, drawable_height_);

    if (zoom_left_) {
      SDL_RenderDrawLine(renderer_, dst_half_zoomed_size, drawable_height_ - dst_zoomed_size, dst_half_zoomed_size, drawable_height_);
    }
    if (zoom_right_) {
      SDL_RenderDrawLine(renderer_, drawable_width_ - dst_half_zoomed_size, drawable_height_ - dst_zoomed_size, drawable_width_ - dst_half_zoomed_size, drawable_height_);
    }
  }

  SDL_RenderPresent(renderer_);
}

void Display::input() {
  SDL_GetMouseState(&mouse_x_, &mouse_y_);

  seek_relative_ = 0.0F;
  seek_from_start_ = false;
  frame_offset_delta_ = 0;
  shift_right_frames_ = 0;

  while (SDL_PollEvent(&event_) != 0) {
    switch (event_.type) {
      case SDL_WINDOWEVENT:
        switch (event_.window.event) {
          case SDL_WINDOWEVENT_LEAVE:
            mouse_is_inside_window_ = false;
            break;
          case SDL_WINDOWEVENT_ENTER:
            mouse_is_inside_window_ = true;
            break;
        }
        break;
      case SDL_MOUSEBUTTONDOWN:
        seek_relative_ = static_cast<float>(mouse_x_) / static_cast<float>(window_width_);
        seek_from_start_ = true;
        break;
      case SDL_KEYDOWN:
        switch (event_.key.keysym.sym) {
          case SDLK_ESCAPE:
            quit_ = true;
            break;
          case SDLK_SPACE:
            play_ = !play_;
            break;
          case SDLK_1:
          case SDLK_KP_1:
            show_left_ = !show_left_;
            break;
          case SDLK_2:
          case SDLK_KP_2:
            show_right_ = !show_right_;
            break;
          case SDLK_3:
          case SDLK_KP_3:
            show_hud_ = !show_hud_;
            break;
          case SDLK_0:
          case SDLK_KP_0:
            subtraction_mode_ = !subtraction_mode_;
            break;
          case SDLK_z:
            zoom_left_ = true;
            break;
          case SDLK_c:
            zoom_right_ = true;
            break;
          case SDLK_a:
            frame_offset_delta_++;
            break;
          case SDLK_d:
            frame_offset_delta_--;
            break;
          case SDLK_s: {
            swap_left_right_ = !swap_left_right_;

            SDL_Texture* temp = left_text_texture_;
            left_text_texture_ = right_text_texture_;
            right_text_texture_ = temp;

            int temp_dim = left_text_width_;
            left_text_width_ = right_text_width_;
            right_text_width_ = temp_dim;

            temp_dim = left_text_height_;
            left_text_height_ = right_text_height_;
            right_text_height_ = temp_dim;
            break;
          }
          case SDLK_f:
            save_image_frames_ = true;
            break;
          case SDLK_p:
            print_mouse_position_and_color_ = true;
            break;
          case SDLK_LEFT:
            seek_relative_ -= 1.0F;
            break;
          case SDLK_DOWN:
            seek_relative_ -= 10.0F;
            break;
          case SDLK_PAGEDOWN:
            seek_relative_ -= 600.0F;
            break;
          case SDLK_RIGHT:
            seek_relative_ += 1.0F;
            break;
          case SDLK_UP:
            seek_relative_ += 10.0F;
            break;
          case SDLK_PAGEUP:
            seek_relative_ += 600.0F;
            break;
          case SDLK_PLUS:
          case SDLK_KP_PLUS:
            if (event_.key.keysym.mod & KMOD_ALT) {
              shift_right_frames_ += 100;
            } else if (event_.key.keysym.mod & KMOD_CTRL) {
              shift_right_frames_ += 10;
            } else {
              shift_right_frames_++;
            }
            break;
          case SDLK_MINUS:
          case SDLK_KP_MINUS:
            if (event_.key.keysym.mod & KMOD_ALT) {
              shift_right_frames_ -= 100;
            } else if (event_.key.keysym.mod & KMOD_CTRL) {
              shift_right_frames_ -= 10;
            } else {
              shift_right_frames_--;
            }
            break;
          default:
            break;
        }
        break;
      case SDL_KEYUP:
        switch (event_.key.keysym.sym) {
          case SDLK_z:
            zoom_left_ = false;
            break;
          case SDLK_c:
            zoom_right_ = false;
            break;
        }
        break;
      case SDL_QUIT:
        quit_ = true;
        break;
      default:
        break;
    }
  }
}

bool Display::get_quit() const {
  return quit_;
}

bool Display::get_play() const {
  return play_;
}

bool Display::get_swap_left_right() const {
  return swap_left_right_;
}

float Display::get_seek_relative() const {
  return seek_relative_;
}

bool Display::get_seek_from_start() const {
  return seek_from_start_;
}

int Display::get_frame_offset_delta() const {
  return frame_offset_delta_;
}

int Display::get_shift_right_frames() const {
  return shift_right_frames_;
}
