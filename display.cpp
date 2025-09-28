#include "display.h"
#include <libgen.h>
#include <algorithm>
#include <atomic>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include "controls.h"
#include "ffmpeg.h"
#include "format_converter.h"
#include "png_saver.h"
#include "source_code_pro_regular_ttf.h"
#include "version.h"
#include "video_compare_icon.h"
#include "vmaf_calculator.h"
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

static const SDL_Color BACKGROUND_COLOR = {54, 69, 79, 0};
static const SDL_Color LOOP_OFF_LABEL_COLOR = {0, 0, 0, 0};
static const SDL_Color LOOP_FW_LABEL_COLOR = {80, 127, 255, 0};
static const SDL_Color LOOP_PP_LABEL_COLOR = {191, 95, 60, 0};
static const SDL_Color TEXT_COLOR = {255, 255, 255, 0};
static const SDL_Color HELP_TEXT_PRIMARY_COLOR = {255, 255, 255, 0};
static const SDL_Color HELP_TEXT_ALTERNATE_COLOR = {255, 255, 192, 0};
static const SDL_Color POSITION_COLOR = {255, 255, 192, 0};
static const SDL_Color TARGET_COLOR = {200, 200, 140, 0};
static const SDL_Color ZOOM_COLOR = {255, 165, 0, 0};
static const SDL_Color PLAYBACK_SPEED_COLOR = {0, 192, 160, 0};
static const SDL_Color BUFFER_COLOR = {160, 225, 192, 0};
static const int BACKGROUND_ALPHA = 100;

static const int MOUSE_WHEEL_SCROLL_STEPS_TO_DOUBLE = 12;
static const float ZOOM_STEP_SIZE = pow(2.0F, 1.0F / float(MOUSE_WHEEL_SCROLL_STEPS_TO_DOUBLE));
static const int PLAYBACK_SPEED_KEY_PRESSES_TO_DOUBLE = 6;
static const float PLAYBACK_SPEED_STEP_SIZE = pow(2.0F, 1.0F / float(PLAYBACK_SPEED_KEY_PRESSES_TO_DOUBLE));

static const int HELP_TEXT_LINE_SPACING = 1;
static const int HELP_TEXT_HORIZONTAL_MARGIN = 26;

auto frame_deleter = [](AVFrame* frame) {
  av_freep(&frame->data[0]);
  av_frame_free(&frame);
};
using AVFramePtr = std::unique_ptr<AVFrame, decltype(frame_deleter)>;

template <typename T>
inline T check_sdl(T value, const std::string& message) {
  if (!value) {
    throw std::runtime_error{"SDL " + message + " - " + SDL_GetError()};
  }
  return value;
}

template <typename T>
inline T clamp_range(T v, T lo, T hi) {
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

inline uint32_t clamp_u32(int v, uint32_t hi) {
  return (v < 0) ? 0u : (v > (int)hi ? hi : (uint32_t)v);
}

inline int clamp_int_to_byte_range(int value) {
  return clamp_range(value, 0, 255);
}

inline int clamp_int_to_10_bpc_range(int value) {
  return clamp_range(value, 0, 1023);
}

inline uint8_t clamp_int_to_byte(int value) {
  return static_cast<uint8_t>(clamp_int_to_byte_range(value));
}

inline uint16_t clamp_int_to_10_bpc(int value) {
  return static_cast<uint16_t>(clamp_int_to_10_bpc_range(value));
}

inline int luma709(int r, int g, int b) {
  return (217 * r + 733 * g + 74 * b) >> 10;
}

template <int Bpc>
struct BitDepthTraits;
template <>
struct BitDepthTraits<8> {
  using P = uint8_t;
  static constexpr uint32_t MaxCode = 255u;
  static constexpr int PackShift = 0;          // stored as 8b
  static inline int to10(int v) { return v; }  // already 8-bit working domain
  static inline P from10(uint32_t v) { return (P)clamp_u32((int)v, MaxCode); }
};

template <>
struct BitDepthTraits<10> {
  using P = uint16_t;
  static constexpr uint32_t MaxCode = 1023u;
  static constexpr int PackShift = 6;          // stored as 16b with <<6
  static inline int to10(int v) { return v; }  // values in working domain are 10b
  static inline P from10(uint32_t v) { return (P)(clamp_u32((int)v, MaxCode) << PackShift); }
};

// Credits to Kemin Zhou for this approach which does not require Boost or C++17
// https://stackoverflow.com/questions/4430780/how-can-i-extract-the-file-name-and-extension-from-a-path-in-c
std::string get_file_name_and_extension(const std::string& file_path) {
  char* buff = new char[file_path.size() + 1];
  strcpy(buff, file_path.c_str());

  const std::string result = std::string(basename(buff));

  delete[] buff;

  return result;
}

std::string get_file_stem(const std::string& file_path) {
  std::string tmp = get_file_name_and_extension(file_path);

  const std::string::size_type i = tmp.rfind('.');

  if (i != std::string::npos) {
    tmp = tmp.substr(0, i);
  }

  return tmp;
}

std::string strip_ffmpeg_patterns(const std::string& input) {
  static const std::regex pattern_regex(R"(%\d*d|\*|\?)");

  return std::regex_replace(input, pattern_regex, "");
};

inline float round_3(float value) {
  return std::round(value * 1000.0F) / 1000.0F;
}

static std::string format_position_difference(const float position1, const float position2) {
  // round both for the sake of consistency with the displayed positions
  const float position1_rounded = round_3(position1);
  const float position2_rounded = round_3(position2);

  // absolute difference very close to 0.001 -> we are in sync!
  if (std::abs(position1_rounded - position2_rounded) < 9.99e-4) {
    return "";
  } else if (position1 < position2) {
    return " (-" + format_position(position2_rounded - position1_rounded, true) + ")";
  }

  return " (+" + format_position(position1_rounded - position2_rounded, true) + ")";
}

static std::string to_hex(const uint32_t value, const int width) {
  std::stringstream sstream;
  sstream << std::setfill('0') << std::setw(width) << std::hex << value;

  return sstream.str();
}

static std::string format_libav_version(unsigned version) {
  int major = (version >> 16) & 0xff;
  int minor = (version >> 8) & 0xff;
  int micro = version & 0xff;
  return string_sprintf("%2u.%2u.%3u", major, minor, micro);
}

auto get_metadata_int_value = [](const AVFrame* frame, const std::string& key, const int default_value) -> int {
  const AVDictionaryEntry* entry = av_dict_get(frame->metadata, key.c_str(), nullptr, 0);

  return entry ? std::atoi(entry->value) : default_value;
};

SDL::SDL() {
  check_sdl(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0, "SDL init");
  check_sdl(TTF_Init() == 0, "TTF init");
}

SDL::~SDL() {
  SDL_Quit();
}

Display::Display(const int display_number,
                 const Mode mode,
                 const bool verbose,
                 const bool fit_window_to_usable_bounds,
                 const bool high_dpi_allowed,
                 const bool use_10_bpc,
                 const bool fast_input_alignment,
                 const bool bilinear_texture_filtering,
                 const std::tuple<int, int> window_size,
                 const unsigned width,
                 const unsigned height,
                 const double duration,
                 const float wheel_sensitivity,
                 const std::string& left_file_name,
                 const std::string& right_file_name)
    : display_number_{display_number},
      mode_{mode},
      fit_window_to_usable_bounds_{fit_window_to_usable_bounds},
      high_dpi_allowed_{high_dpi_allowed},
      use_10_bpc_{use_10_bpc},
      fast_input_alignment_{fast_input_alignment},
      bilinear_texture_filtering_{bilinear_texture_filtering},
      video_width_{static_cast<int>(width)},
      video_height_{static_cast<int>(height)},
      duration_{duration},
      wheel_sensitivity_{wheel_sensitivity},
      left_file_stem_{strip_ffmpeg_patterns(get_file_stem(left_file_name))},
      right_file_stem_{strip_ffmpeg_patterns(get_file_stem(right_file_name))} {
  const int auto_width = mode == Mode::HSTACK ? width * 2 : width;
  const int auto_height = mode == Mode::VSTACK ? height * 2 : height;

  int window_x;
  int window_y;
  int window_width;
  int window_height;

  constexpr int min_width = 4;
  constexpr int min_height = 1;

  if (!fit_window_to_usable_bounds) {
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

    window_x = SDL_WINDOWPOS_UNDEFINED_DISPLAY(display_number);
    window_y = SDL_WINDOWPOS_UNDEFINED_DISPLAY(display_number);

    if (high_dpi_allowed_) {
      window_width /= 2;
      window_height /= 2;
    }
  } else {
    SDL_Rect bounds;
    check_sdl(SDL_GetDisplayUsableBounds(display_number, &bounds) == 0, "get display usable bounds");

    // account for window frame and title bar
    constexpr int border_width = 10;
#ifdef __linux__
    constexpr int border_height = 40;
#else
    constexpr int border_height = 34;
#endif

    const int usable_width = std::max(bounds.w - border_width, min_width);
    const int usable_height = std::max(bounds.h - border_height, min_height);

    const float aspect_ratio = static_cast<float>(auto_width) / static_cast<float>(auto_height);
    const float usable_aspect_ratio = static_cast<float>(usable_width) / static_cast<float>(usable_height);

    if (usable_aspect_ratio > aspect_ratio) {
      window_height = usable_height;
      window_width = static_cast<int>(window_height * aspect_ratio);
    } else {
      window_width = usable_width;
      window_height = static_cast<int>(window_width / aspect_ratio);
    }

    window_x = bounds.x + (usable_width - window_width + border_width) / 2;
    window_y = bounds.y + (usable_height - window_height + border_height) / 2 + border_width;
#ifdef __linux__
    window_y -= 2 * border_width + 4;
#endif
  }

  if (window_width < min_width) {
    throw std::runtime_error{"Window width cannot be less than " + std::to_string(min_width)};
  }
  if (window_height < min_height) {
    throw std::runtime_error{"Window height cannot be less than " + std::to_string(min_height)};
  }

  const int create_window_flags = SDL_WINDOW_SHOWN;
  window_ = check_sdl(SDL_CreateWindow(string_sprintf("%s  |  %s", get_file_name_and_extension(left_file_name).c_str(), get_file_name_and_extension(right_file_name).c_str()).c_str(), window_x, window_y, window_width, window_height,
                                       high_dpi_allowed_ ? create_window_flags | SDL_WINDOW_ALLOW_HIGHDPI : create_window_flags),
                      "window");

  SDL_RWops* embedded_icon = check_sdl(SDL_RWFromConstMem(VIDEO_COMPARE_ICON_BMP, VIDEO_COMPARE_ICON_BMP_LEN), "get pointer to icon");
  SDL_Surface* icon_surface = check_sdl(SDL_LoadBMP_RW(embedded_icon, 1), "load icon");

#ifdef _WIN32
  SDL_Surface* resized_icon_surface = SDL_CreateRGBSurface(0, 64, 64, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
  SDL_BlitScaled(icon_surface, nullptr, resized_icon_surface, nullptr);
  SDL_SetWindowIcon(window_, resized_icon_surface);
  SDL_FreeSurface(resized_icon_surface);
#else
  SDL_SetWindowIcon(window_, icon_surface);
#endif

  SDL_FreeSurface(icon_surface);

  renderer_ = check_sdl(SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC), "renderer");

  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
  SDL_RenderClear(renderer_);
  SDL_RenderPresent(renderer_);

  SDL_GL_GetDrawableSize(window_, &drawable_width_, &drawable_height_);
  SDL_GetWindowSize(window_, &window_width_, &window_height_);

  drawable_to_window_width_factor_ = static_cast<float>(drawable_width_) / static_cast<float>(window_width_);
  drawable_to_window_height_factor_ = static_cast<float>(drawable_height_) / static_cast<float>(window_height_);
  video_to_window_width_factor_ = static_cast<float>(video_width_) / static_cast<float>(window_width_) * ((mode_ == Mode::HSTACK) ? 2.F : 1.F);
  video_to_window_height_factor_ = static_cast<float>(video_height_) / static_cast<float>(window_height_) * ((mode_ == Mode::VSTACK) ? 2.F : 1.F);

  font_scale_ = (drawable_to_window_width_factor_ + drawable_to_window_height_factor_) / 2.0F;

  border_extension_ = 3 * font_scale_;
  double_border_extension_ = border_extension_ * 2;
  line1_y_ = 20;
  line2_y_ = line1_y_ + 30 * font_scale_;

  if (mode_ != Mode::VSTACK) {
    max_text_width_ = drawable_width_ / 2 - double_border_extension_ - line1_y_;
  } else {
    max_text_width_ = drawable_width_ - double_border_extension_ - line1_y_;
  }

  SDL_RWops* embedded_font = check_sdl(SDL_RWFromConstMem(SOURCE_CODE_PRO_REGULAR_TTF, SOURCE_CODE_PRO_REGULAR_TTF_LEN), "get pointer to font");
  small_font_ = check_sdl(TTF_OpenFontRW(embedded_font, 0, 16 * font_scale_), "font open");
  big_font_ = check_sdl(TTF_OpenFontRW(embedded_font, 0, 24 * font_scale_), "font open");

  normal_mode_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
  pan_mode_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEALL);
  selection_mode_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);

  SDL_RenderSetLogicalSize(renderer_, drawable_width_, drawable_height_);

  auto create_video_texture = [&](const std::string& scale_quality) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scale_quality.c_str());

    return check_sdl(SDL_CreateTexture(renderer_, use_10_bpc ? SDL_PIXELFORMAT_ARGB2101010 : SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, mode == Mode::HSTACK ? width * 2 : width, mode == Mode::VSTACK ? height * 2 : height),
                     "video texture " + scale_quality);
  };

  video_texture_linear_ = create_video_texture("linear");
  video_texture_nn_ = create_video_texture("nearest");

  if (verbose) {
    print_verbose_info();
  }

  auto render_text_with_fallback = [&](const std::string& text) {
    SDL_Surface* surface = TTF_RenderUTF8_Blended(small_font_, text.c_str(), TEXT_COLOR);

    if (!surface) {
      std::cerr << "Falling back to lower-quality rendering for '" << text << "'" << std::endl;

      surface = check_sdl(TTF_RenderUTF8_Solid(small_font_, text.c_str(), TEXT_COLOR), "text surface");
    }

    return surface;
  };

  SDL_Surface* text_surface = render_text_with_fallback(left_file_name);
  left_text_texture_ = SDL_CreateTextureFromSurface(renderer_, text_surface);
  left_text_width_ = text_surface->w;
  left_text_height_ = text_surface->h;
  SDL_FreeSurface(text_surface);

  text_surface = render_text_with_fallback(right_file_name);
  right_text_texture_ = SDL_CreateTextureFromSurface(renderer_, text_surface);
  right_text_width_ = text_surface->w;
  right_text_height_ = text_surface->h;
  SDL_FreeSurface(text_surface);

  diff_buffer_ = new uint8_t[video_width_ * video_height_ * 3 * (use_10_bpc ? sizeof(uint16_t) : sizeof(uint8_t))];
  uint8_t* diff_plane_0 = diff_buffer_;

  diff_planes_ = {diff_plane_0, nullptr, nullptr};
  diff_pitches_ = {video_width_ * 3 * (use_10_bpc ? sizeof(uint16_t) : sizeof(uint8_t)), 0, 0};

  // initialize help texts
  bool primary_color = true;

  auto add_help_texture = [&](TTF_Font* font, const std::string& text) {
    int h;

    SDL_Surface* surface = TTF_RenderUTF8_Blended_Wrapped(font, text.c_str(), primary_color ? HELP_TEXT_PRIMARY_COLOR : HELP_TEXT_ALTERNATE_COLOR, drawable_width_ - HELP_TEXT_HORIZONTAL_MARGIN * 2);
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_FreeSurface(surface);

    SDL_QueryTexture(texture, nullptr, nullptr, nullptr, &h);
    help_total_height_ += h;

    help_textures_.push_back(texture);
  };

  add_help_texture(small_font_, " ");
  TTF_SetFontStyle(big_font_, TTF_STYLE_BOLD | TTF_STYLE_UNDERLINE);
  add_help_texture(big_font_, "CONTROLS");
  TTF_SetFontStyle(big_font_, TTF_STYLE_NORMAL);
  add_help_texture(small_font_, " ");

  for (auto& key_description_pair : get_controls()) {
    primary_color = !primary_color;
    add_help_texture(small_font_, string_sprintf(" %-12s %s", key_description_pair.first.c_str(), key_description_pair.second.c_str()));
  }

  add_help_texture(big_font_, " ");

  for (auto& text : get_instructions()) {
    primary_color = !primary_color;
    add_help_texture(small_font_, text);
    add_help_texture(small_font_, " ");
  }
}

Display::~Display() {
  SDL_DestroyTexture(video_texture_linear_);
  SDL_DestroyTexture(video_texture_nn_);
  SDL_DestroyTexture(left_text_texture_);
  SDL_DestroyTexture(right_text_texture_);

  if (message_texture_ != nullptr) {
    SDL_DestroyTexture(message_texture_);
  }

  for (auto help_texture : help_textures_) {
    SDL_DestroyTexture(help_texture);
  }

  TTF_CloseFont(small_font_);
  TTF_CloseFont(big_font_);

  SDL_FreeCursor(normal_mode_cursor_);
  SDL_FreeCursor(pan_mode_cursor_);
  SDL_FreeCursor(selection_mode_cursor_);

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

void Display::print_verbose_info() {
  std::cout << "Main program version:  " << VersionInfo::version << std::endl;
  std::cout << "Video size:            " << video_width_ << "x" << video_height_ << std::endl;
  std::cout << "Video duration:        " << format_duration(duration_) << std::endl;
  std::cout << "Display mode:          " << modeToString(mode_) << std::endl;
  std::cout << "Fit to usable bounds:  " << std::boolalpha << fit_window_to_usable_bounds_ << std::endl;
  std::cout << "High-DPI allowed:      " << std::boolalpha << high_dpi_allowed_ << std::endl;
  std::cout << "Use 10 bpc:            " << std::boolalpha << use_10_bpc_ << std::endl;
  std::cout << "Fast input alignment:  " << std::boolalpha << fast_input_alignment_ << std::endl;
  std::cout << "Mouse whl sensitivity: " << wheel_sensitivity_ << std::endl;

  SDL_version sdl_linked_version;
  SDL_GetVersion(&sdl_linked_version);
  std::cout << "SDL version:           " << string_sprintf("%u.%u.%u", sdl_linked_version.major, sdl_linked_version.minor, sdl_linked_version.patch) << std::endl;

  const SDL_version* sdl_ttf_linked_version = TTF_Linked_Version();
  std::cout << "SDL_ttf version:       " << string_sprintf("%u.%u.%u", sdl_ttf_linked_version->major, sdl_ttf_linked_version->minor, sdl_ttf_linked_version->patch) << std::endl;

  SDL_RendererInfo info;
  SDL_GetRendererInfo(renderer_, &info);
  std::cout << "SDL renderer:          " << info.name << std::endl;

  int current_display_number = SDL_GetWindowDisplayIndex(window_);
  std::cout << "SDL display number:    " << current_display_number << std::endl;

  SDL_DisplayMode desktop_display_mode;
  SDL_GetDesktopDisplayMode(current_display_number, &desktop_display_mode);
  std::cout << "SDL desktop size:      " << desktop_display_mode.w << "x" << desktop_display_mode.h << std::endl;

  std::cout << "SDL GL drawable size:  " << drawable_width_ << "x" << drawable_height_ << std::endl;
  std::cout << "SDL window size:       " << window_width_ << "x" << window_height_ << std::endl;

  auto stringify_format_and_bpp = [&](Uint32 pixel_format) -> std::string { return string_sprintf("%s (%d bpp)", SDL_GetPixelFormatName(pixel_format), SDL_BITSPERPIXEL(pixel_format)); };

  Uint32 window_pixel_format = SDL_GetWindowPixelFormat(window_);
  std::cout << "SDL window px format:  " << stringify_format_and_bpp(window_pixel_format) << std::endl;

  Uint32 video_pixel_format;
  SDL_QueryTexture(video_texture_linear_, &video_pixel_format, nullptr, nullptr, nullptr);
  std::cout << "SDL video px format:   " << stringify_format_and_bpp(video_pixel_format) << std::endl;

  std::cout << "FFmpeg version:        " << av_version_info() << std::endl;
  std::cout << "libavutil version:     " << format_libav_version(avutil_version()) << std::endl;
  std::cout << "libavcodec version:    " << format_libav_version(avcodec_version()) << std::endl;
  std::cout << "libavformat version:   " << format_libav_version(avformat_version()) << std::endl;
  std::cout << "libavfilter version:   " << format_libav_version(avfilter_version()) << std::endl;
  std::cout << "libswscale version:    " << format_libav_version(swscale_version()) << std::endl;
  std::cout << "libswresample version: " << format_libav_version(swresample_version()) << std::endl;
  std::cout << "libavcodec configuration: " << avcodec_configuration() << std::endl << std::endl;
}

void Display::convert_to_packed_10_bpc(std::array<uint8_t*, 3> in_planes, std::array<size_t, 3> in_pitches, std::array<uint32_t*, 3> out_planes, std::array<size_t, 3> out_pitches, const SDL_Rect& roi) {
  row_workers_.run_dynamic(
      roi.h,
      [=](const int start_row, const int end_row) {
        uint16_t* p_in = reinterpret_cast<uint16_t*>(in_planes[0] + roi.x * 6 + in_pitches[0] * (roi.y + start_row));
        uint32_t* p_out = out_planes[0] + roi.x + out_pitches[0] * (roi.y + start_row) / sizeof(uint32_t);

        for (int y = start_row; y < end_row; y++) {
          for (int in_x = 0, out_x = 0; out_x < roi.w; in_x += 3, out_x++) {
            const uint32_t r = p_in[in_x] >> 6;
            const uint32_t g = p_in[in_x + 1] >> 6;
            const uint32_t b = p_in[in_x + 2] >> 6;

            p_out[out_x] = (r << 20) | (g << 10) | (b);
          }

          p_in += in_pitches[0] / sizeof(uint16_t);
          p_out += out_pitches[0] / sizeof(uint32_t);
        }
      },
      suggest_block_rows_by_bytes(roi.w, roi.h, sizeof(uint16_t), 3));
}

template <int Bpc>
inline void process_difference_scanline(const typename BitDepthTraits<Bpc>::P* plane_left,
                                        const typename BitDepthTraits<Bpc>::P* plane_right,
                                        typename BitDepthTraits<Bpc>::P* plane_difference,
                                        const int pixels,
                                        const Display::DiffMode mode,
                                        const bool luma_only,
                                        const std::vector<uint32_t>& mag_u,
                                        const std::vector<uint32_t>& mag_s) {
  using T = BitDepthTraits<Bpc>;
  constexpr uint32_t MAX = T::MaxCode;
  constexpr uint32_t MID = MAX >> 1;

  auto load = [](typename T::P v) -> int { return (int)(v >> T::PackShift); };

  for (int i = 0; i < pixels; i++) {
    const int idx = i * 3;
    const int rl = load(plane_left[idx]), gl = load(plane_left[idx + 1]), bl = load(plane_left[idx + 2]);
    const int rr = load(plane_right[idx]), gr = load(plane_right[idx + 1]), br = load(plane_right[idx + 2]);

    if (mode == Display::DiffMode::LegacyAbs) {
      // Original: per-channel abs * AMPLIFICATION, clamped to bit depth
      constexpr int AMPLIFICATION = 2;

      if (luma_only) {
        const int dl = luma709(rl, gl, bl) - luma709(rr, gr, br);
        const uint32_t Y = clamp_u32(std::abs(dl) * AMPLIFICATION, MAX);
        auto y_p = T::from10(Y);

        plane_difference[idx] = y_p;
        plane_difference[idx + 1] = y_p;
        plane_difference[idx + 2] = y_p;
      } else {
        const uint32_t R = clamp_u32(std::abs(rl - rr) * AMPLIFICATION, MAX);
        const uint32_t G = clamp_u32(std::abs(gl - gr) * AMPLIFICATION, MAX);
        const uint32_t B = clamp_u32(std::abs(bl - br) * AMPLIFICATION, MAX);

        plane_difference[idx + 0] = T::from10(R);
        plane_difference[idx + 1] = T::from10(G);
        plane_difference[idx + 2] = T::from10(B);
      }
      continue;
    }

    // Adaptive mapping with optional sign and luma-only
    if (luma_only) {
      const int dl = luma709(rl, gl, bl) - luma709(rr, gr, br);
      const uint32_t a = (uint32_t)std::min<int>(MAX, std::abs(dl));

      if (mode == Display::DiffMode::SignedDiverging) {
        const uint32_t m = mag_s[a];
        const uint32_t Y = (dl >= 0) ? (MID + m) : (MID - m);
        auto y_p = T::from10(Y);
        plane_difference[idx + 0] = plane_difference[idx + 1] = plane_difference[idx + 2] = y_p;
      } else {
        const uint32_t Y = mag_u[a];
        auto y_p = T::from10(Y);
        plane_difference[idx + 0] = plane_difference[idx + 1] = plane_difference[idx + 2] = y_p;
      }
    } else {
      const int dr = rl - rr, dg = gl - gr, db = bl - br;

      if (mode == Display::DiffMode::SignedDiverging) {
        const uint32_t ar = (uint32_t)std::min<int>(MAX, std::abs(dr));
        const uint32_t ag = (uint32_t)std::min<int>(MAX, std::abs(dg));
        const uint32_t ab = (uint32_t)std::min<int>(MAX, std::abs(db));

        plane_difference[idx + 0] = T::from10(dr >= 0 ? (MID + mag_s[ar]) : (MID - mag_s[ar]));
        plane_difference[idx + 1] = T::from10(dg >= 0 ? (MID + mag_s[ag]) : (MID - mag_s[ag]));
        plane_difference[idx + 2] = T::from10(db >= 0 ? (MID + mag_s[ab]) : (MID - mag_s[ab]));
      } else {
        const uint32_t ar = (uint32_t)std::min<int>(MAX, std::abs(dr));
        const uint32_t ag = (uint32_t)std::min<int>(MAX, std::abs(dg));
        const uint32_t ab = (uint32_t)std::min<int>(MAX, std::abs(db));

        plane_difference[idx + 0] = T::from10(mag_u[ar]);
        plane_difference[idx + 1] = T::from10(mag_u[ag]);
        plane_difference[idx + 2] = T::from10(mag_u[ab]);
      }
    }
  }
}

template <int Bpc>
float Display::calculate_frame_p99(const typename BitDepthTraits<Bpc>::P* plane_left, const typename BitDepthTraits<Bpc>::P* plane_right, const size_t pitch_left, const size_t pitch_right, const int width_right) const {
  using T = BitDepthTraits<Bpc>;
  static_assert(Bpc == 8 || Bpc == 10, "Bpc must be 8 or 10");
  constexpr int CHANNELS = 3;

  const size_t stride_l = pitch_left / sizeof(typename T::P);
  const size_t stride_r = pitch_right / sizeof(typename T::P);

  const int bins = static_cast<int>(T::MaxCode) + 1;
  const int num_threads = row_workers_.size();

  std::vector<std::vector<uint32_t>> thread_histograms(num_threads, std::vector<uint32_t>(bins, 0u));

  // Use RowWorkers to compute histograms for different row ranges
  auto histograms_ptr = std::make_shared<std::vector<std::vector<uint32_t>>>(std::move(thread_histograms));

  row_workers_.run_dynamic_indexed(
      video_height_,
      [=](const int start_row, const int end_row, const int worker_index) {
        auto& hist = (*histograms_ptr)[worker_index];

        for (int y = start_row; y < end_row; y++) {
          const typename T::P* row_l = plane_left + y * stride_l;
          const typename T::P* row_r = plane_right + y * stride_r;

          for (int x = 0; x < width_right; x++) {
            const int idx = x * CHANNELS;

            const int rl = row_l[idx + 0] >> T::PackShift;
            const int gl = row_l[idx + 1] >> T::PackShift;
            const int bl = row_l[idx + 2] >> T::PackShift;

            const int rr = row_r[idx + 0] >> T::PackShift;
            const int gr = row_r[idx + 1] >> T::PackShift;
            const int br = row_r[idx + 2] >> T::PackShift;

            int d;
            if (diff_luma_only_) {
              const int yl = luma709(rl, gl, bl);
              const int yr = luma709(rr, gr, br);
              d = std::abs(yl - yr);
            } else {
              const int dr = std::abs(rl - rr);
              const int dg = std::abs(gl - gr);
              const int db = std::abs(bl - br);
              d = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
            }

            const int bin = clamp_range(d, 0, bins - 1);
            hist[static_cast<size_t>(bin)]++;
          }
        }
      },
      suggest_block_rows_by_bytes(video_width_, video_height_, sizeof(typename BitDepthTraits<Bpc>::P), 3));

  // Merge histograms
  std::vector<uint32_t> hist(bins, 0u);
  for (const auto& thread_hist : *histograms_ptr) {
    for (size_t i = 0; i < bins; ++i) {
      hist[i] += thread_hist[i];
    }
  }

  // Sum of histogram counts
  uint64_t total = std::accumulate(hist.begin(), hist.end(), 0);

  if (total == 0) {
    return 1.f;
  }

  // Linear-interpolated 99th percentile
  const double target_f = 0.99 * (double)(total - 1);
  const uint64_t r0 = (uint64_t)std::floor(target_f);
  const uint64_t r1 = (uint64_t)std::ceil(target_f);
  const double frac = target_f - (double)r0;

  int v0 = bins - 1, v1 = bins - 1;
  uint64_t acc = 0;

  // Find the values at ranks r0 and r1
  for (int k = 0; k < bins; k++) {
    const uint64_t next = acc + hist[static_cast<size_t>(k)];
    if (acc <= r0 && r0 < next) {
      v0 = k;
    }
    if (acc <= r1 && r1 < next) {
      v1 = k;
      break;
    }
    acc = next;
  }

  const float p = (float)v0 + frac * (float)(v1 - v0);
  return p;
}

std::pair<std::vector<uint32_t>, std::vector<uint32_t>> make_diff_lut(uint32_t max_code, Display::DiffMode mode, uint32_t scale_max) {
  std::vector<uint32_t> mag_u(max_code + 1);
  std::vector<uint32_t> mag_s(max_code + 1);

  if (mode != Display::DiffMode::LegacyAbs) {
    if (scale_max == 0) {
      scale_max = 1;
    }

    const uint32_t MID = max_code >> 1;
    const uint32_t Q = 16;
    const uint32_t ONE_Q = 1u << Q;
    const uint64_t HALF = uint64_t(1) << (Q - 1);

    for (uint32_t a = 0; a <= max_code; a++) {
      // x_q = clamp(a/scale, 0..1) in Q16
      uint32_t x_q = (uint32_t)std::min<uint64_t>(ONE_Q, ((uint64_t)a << Q) / scale_max);

      // map_unit(x): Linear / Sqrt (SignedDiverging uses sqrt magnitude)
      uint32_t y_q;
      switch (mode) {
        case Display::DiffMode::AbsLinear:
          y_q = x_q;
          break;
        case Display::DiffMode::AbsSqrt:
        case Display::DiffMode::SignedDiverging: {
          const double x = double(x_q) / double(ONE_Q);
          const double y = std::sqrt(x);
          y_q = (uint32_t)std::llround(y * double(ONE_Q));
          break;
        }
        default:  // LegacyAbs not expected here; fall back to linear
          y_q = x_q;
          break;
      }

      // Scale back to code domain with Q16 rounding
      mag_u[a] = (uint32_t)(((uint64_t)y_q * max_code + HALF) >> Q);  // [0..MAX]
      mag_s[a] = (uint32_t)(((uint64_t)y_q * MID + HALF) >> Q);       // [0..MID]
    }
  }

  return std::pair<std::vector<uint32_t>, std::vector<uint32_t>>(std::move(mag_u), std::move(mag_s));
};

template <int Bpc>
void Display::process_difference_planes(const typename BitDepthTraits<Bpc>::P* plane_left0,
                                        const typename BitDepthTraits<Bpc>::P* plane_right0,
                                        typename BitDepthTraits<Bpc>::P* plane_difference0,
                                        const size_t pitch_left,
                                        const size_t pitch_right,
                                        const size_t pitch_difference,
                                        const int width_right,
                                        const float diff_max) const {
  using T = BitDepthTraits<Bpc>;
  constexpr uint32_t MAX = T::MaxCode;

  const float scale_max = (diff_mode_ == Display::DiffMode::LegacyAbs) ? -1.f : clamp_range(diff_max, 4.f, (float)MAX);

  // Integerize/clip scale once
  const uint32_t scale_max_i = (uint32_t)std::max<double>(1.0, std::min<double>(double(MAX), std::round(std::fabs(scale_max))));

  // Build LUTs (only for adaptive mapping)
  auto luts = make_diff_lut(MAX, diff_mode_, scale_max_i);
  const std::vector<uint32_t> mag_u = std::move(luts.first);
  const std::vector<uint32_t> mag_s = std::move(luts.second);

  row_workers_.run_dynamic(
      video_height_,
      [=](const int start_row, const int end_row) {
        auto plane_left = plane_left0 + start_row * (pitch_left / sizeof(typename T::P));
        auto plane_right = plane_right0 + start_row * (pitch_right / sizeof(typename T::P));
        auto plane_difference = plane_difference0 + start_row * (pitch_difference / sizeof(typename T::P));

        for (int y = start_row; y < end_row; y++) {
          process_difference_scanline<Bpc>(plane_left, plane_right, plane_difference, width_right, diff_mode_, diff_luma_only_, mag_u, mag_s);
          plane_left += pitch_left / sizeof(typename T::P);
          plane_right += pitch_right / sizeof(typename T::P);
          plane_difference += pitch_difference / sizeof(typename T::P);
        }
      },
      suggest_block_rows_by_bytes(video_width_, video_height_, sizeof(typename BitDepthTraits<Bpc>::P), 3));
}

void Display::update_difference(std::array<uint8_t*, 3> planes_left, std::array<size_t, 3> pitches_left, std::array<uint8_t*, 3> planes_right, std::array<size_t, 3> pitches_right, int split_x) {
  constexpr int CHANNELS = 3;

  const int width_right = (video_width_ - split_x);
  if (width_right <= 0) {
    return;
  }

  const bool update_frame_max = diff_mode_ != DiffMode::LegacyAbs;
  float frame_max = 1.f;

  // row starts after split_x pixels, i.e., split_x * 3 samples
  if (use_10_bpc_) {
    auto plane_left0 = reinterpret_cast<uint16_t*>(planes_left[0]) + split_x * CHANNELS;
    auto plane_right0 = reinterpret_cast<uint16_t*>(planes_right[0]) + split_x * CHANNELS;
    auto plane_difference0 = reinterpret_cast<uint16_t*>(diff_planes_[0]) + split_x * CHANNELS;

    if (update_frame_max) {
      frame_max = calculate_frame_p99<10>(plane_left0, plane_right0, pitches_left[0], pitches_right[0], width_right);
    }

    process_difference_planes<10>(plane_left0, plane_right0, plane_difference0, pitches_left[0], pitches_right[0], diff_pitches_[0], width_right, frame_max);
  } else {
    auto plane_left0 = planes_left[0] + split_x * CHANNELS;
    auto plane_right0 = planes_right[0] + split_x * CHANNELS;
    auto plane_difference0 = diff_planes_[0] + split_x * CHANNELS;

    if (update_frame_max) {
      frame_max = calculate_frame_p99<8>(plane_left0, plane_right0, pitches_left[0], pitches_right[0], width_right);
    }

    process_difference_planes<8>(plane_left0, plane_right0, plane_difference0, pitches_left[0], pitches_right[0], diff_pitches_[0], width_right, frame_max);
  }
}

void write_png(const AVFrame* frame, const std::string& filename, std::atomic_bool& error_occurred) {
  try {
    PngSaver::save(frame, filename);
  } catch (const PngSaver::IOException& e) {
    std::cerr << "Error saving video PNG image to file: " << filename << std::endl;
    error_occurred = true;
  } catch (const std::runtime_error& e) {
    std::cerr << "Unexpected while error saving PNG: " << e.what() << std::endl;
    error_occurred = true;
  }
};

void Display::save_image_frames(const AVFrame* left_frame, const AVFrame* right_frame) {
  std::atomic_bool error_occurred(false);

  const auto create_onscreen_display_avframe = [&]() -> AVFramePtr {
    const size_t pitch = use_10_bpc_ ? drawable_width_ * 3 * sizeof(uint16_t) : drawable_width_ * 3;
    uint8_t* pixels = reinterpret_cast<uint8_t*>(av_malloc(pitch * drawable_height_));

    if (use_10_bpc_) {
      const size_t temp_pitch = drawable_width_ * sizeof(uint32_t);
      std::vector<uint8_t> temp_pixels(temp_pitch * drawable_height_);

      SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_ARGB2101010, temp_pixels.data(), temp_pitch);

      const uint32_t* src = reinterpret_cast<const uint32_t*>(temp_pixels.data());
      uint16_t* dest = reinterpret_cast<uint16_t*>(pixels);

      for (int i = 0; i < drawable_width_ * drawable_height_; i++) {
        const uint32_t argb = *(src++);
        const uint32_t r10 = (argb >> 20) & 0x3FF;
        const uint32_t g10 = (argb >> 10) & 0x3FF;
        const uint32_t b10 = argb & 0x3FF;

        *(dest++) = static_cast<uint16_t>(r10 << 6);
        *(dest++) = static_cast<uint16_t>(g10 << 6);
        *(dest++) = static_cast<uint16_t>(b10 << 6);
      }
    } else {
      SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_RGB24, pixels, pitch);
    }

    AVFrame* renderer_frame = av_frame_alloc();
    renderer_frame->format = use_10_bpc_ ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_RGB24;
    renderer_frame->width = drawable_width_;
    renderer_frame->height = drawable_height_;
    renderer_frame->data[0] = pixels;
    renderer_frame->linesize[0] = pitch;

    return AVFramePtr(renderer_frame, frame_deleter);
  };

  const auto osd_frame = create_onscreen_display_avframe();

  const std::string left_filename = string_sprintf("%s%s_%04d.png", left_file_stem_.c_str(), (left_file_stem_ == right_file_stem_) ? "_left" : "", saved_image_number_);
  const std::string right_filename = string_sprintf("%s%s_%04d.png", right_file_stem_.c_str(), (left_file_stem_ == right_file_stem_) ? "_right" : "", saved_image_number_);
  const std::string osd_filename = string_sprintf("%s_%s_osd_%04d.png", left_file_stem_.c_str(), right_file_stem_.c_str(), saved_image_number_);

  auto save_frame = [&](const AVFrame* frame, const std::string& filename) { return write_png(frame, filename, error_occurred); };

  std::thread save_left_frame_thread(save_frame, left_frame, left_filename);
  std::thread save_right_frame_thread(save_frame, right_frame, right_filename);
  std::thread save_osd_frame_thread(save_frame, osd_frame.get(), osd_filename);

  save_left_frame_thread.join();
  save_right_frame_thread.join();
  save_osd_frame_thread.join();

  if (!error_occurred) {
    std::cout << "Saved " << string_sprintf("%s, %s and %s", left_filename.c_str(), right_filename.c_str(), osd_filename.c_str()) << std::endl;

    saved_image_number_++;
  }
}

void Display::render_text(const int x, const int y, SDL_Texture* texture, const int texture_width, const int texture_height, const int border_extension, const bool left_adjust) {
  // compute clip amount which ensures the filename does not extend more than half the display width
  const int clip_amount = std::max((texture_width + double_border_extension_) - max_text_width_, 0);
  const int gradient_amount = std::min(clip_amount, 24);

  SDL_Rect fill_rect = {x - border_extension + gradient_amount, y - border_extension, texture_width + double_border_extension_ - clip_amount - gradient_amount, texture_height + double_border_extension_};

  SDL_Rect src_rect = {clip_amount + gradient_amount, 0, texture_width - clip_amount - gradient_amount, texture_height};
  SDL_Rect text_rect = {x + gradient_amount, y, texture_width - clip_amount - gradient_amount, texture_height};

  if (!left_adjust && (mode_ != Mode::VSTACK)) {
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

void Display::render_progress_dots(const float position, const float progress, const bool is_top) {
  if (duration_ > 0) {
    const float dot_size = 2.f;

    const int dot_width = std::round(drawable_to_window_width_factor_ * dot_size);
    const int dot_height = std::round(drawable_to_window_height_factor_ * dot_size);

    const int y_offset = is_top ? 1 : drawable_height_ - 1 - dot_height;

    const int x_position = std::round(position * drawable_width_ / duration_);
    const int x_progress = std::round(progress * drawable_width_ / duration_);

    for (int x = 0; x < x_position; x++) {
      if (x % (2 * dot_width) < dot_width) {
        SDL_SetRenderDrawColor(renderer_, POSITION_COLOR.r, POSITION_COLOR.g, POSITION_COLOR.b, BACKGROUND_ALPHA * 3 / 2);
      } else {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA);
      }

      SDL_RenderDrawLine(renderer_, x, y_offset, x, y_offset + dot_height - 1);
    }

    // draw current frame
    SDL_SetRenderDrawColor(renderer_, POSITION_COLOR.r, POSITION_COLOR.g, POSITION_COLOR.b, BACKGROUND_ALPHA * 2);

    const SDL_Rect current_frame = {x_position, is_top ? y_offset : y_offset - dot_height, x_progress - x_position, dot_height * 2};
    SDL_RenderDrawRect(renderer_, &current_frame);
  }
}

SDL_Texture* Display::get_video_texture() const {
  return bilinear_texture_filtering_ ? video_texture_linear_ : video_texture_nn_;
}

void Display::update_texture(const SDL_Rect* rect, const void* pixels, int pitch, const std::string& message) {
  check_sdl(SDL_UpdateTexture(get_video_texture(), rect, pixels, pitch) == 0, "video texture - " + message);
}

int Display::round_and_clamp(const float value) {
  const int result = static_cast<int>(std::roundf(value));

  return use_10_bpc_ ? clamp_int_to_10_bpc_range(result) : clamp_int_to_byte_range(result);
}

const std::array<int, 3> Display::get_rgb_pixel(uint8_t* rgb_plane, const size_t pitch, const int x, const int y) {
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

const std::array<int, 3> Display::convert_rgb_to_yuv(const std::array<int, 3> rgb, const AVPixelFormat rgb_format, const AVColorSpace color_space, const AVColorRange color_range) {
  auto allocate_frame = [&](const AVPixelFormat format) -> AVFramePtr {
    AVFrame* raw_frame = av_frame_alloc();

    if (raw_frame == nullptr) {
      throw ffmpeg::Error("Couldn't allocate frame");
    }

    raw_frame->format = format;
    raw_frame->width = 1;
    raw_frame->height = 1;
    raw_frame->colorspace = color_space;
    raw_frame->color_range = color_range;

    ffmpeg::check(av_image_alloc(raw_frame->data, raw_frame->linesize, raw_frame->width, raw_frame->height, format, 64));

    return AVFramePtr(raw_frame, frame_deleter);
  };

  const AVPixelFormat yuv_format = use_10_bpc_ ? AV_PIX_FMT_YUV444P10 : AV_PIX_FMT_YUV444P;

  auto rgb_pixel_frame = allocate_frame(rgb_format);
  auto yuv_pixel_frame = allocate_frame(yuv_format);

  if (use_10_bpc_) {
    uint16_t* rgb_data = reinterpret_cast<uint16_t*>(rgb_pixel_frame->data[0]);

    auto extend_10_to_16_bit = [](const int value) {
      return (value * 1025) >> 4;  // 1023->65535
    };

    rgb_data[0] = extend_10_to_16_bit(rgb[0]);
    rgb_data[1] = extend_10_to_16_bit(rgb[1]);
    rgb_data[2] = extend_10_to_16_bit(rgb[2]);
  } else {
    uint8_t* rgb_data = reinterpret_cast<uint8_t*>(rgb_pixel_frame->data[0]);

    rgb_data[0] = rgb[0];
    rgb_data[1] = rgb[1];
    rgb_data[2] = rgb[2];
  }

  FormatConverter rgb_to_yuv_converter(1, 1, 1, 1, rgb_format, yuv_format, color_space, color_range);
  rgb_to_yuv_converter(rgb_pixel_frame.get(), yuv_pixel_frame.get());

  if (use_10_bpc_) {
    auto y_data = reinterpret_cast<const uint16_t*>(yuv_pixel_frame->data[0]);
    auto u_data = reinterpret_cast<const uint16_t*>(yuv_pixel_frame->data[1]);
    auto v_data = reinterpret_cast<const uint16_t*>(yuv_pixel_frame->data[2]);

    return {y_data[0], u_data[0], v_data[0]};
  } else {
    return {yuv_pixel_frame->data[0][0], yuv_pixel_frame->data[1][0], yuv_pixel_frame->data[2][0]};
  }
}

std::string Display::format_pixel(const std::array<int, 3>& pixel) {
  std::string hex_pixel = use_10_bpc_ ? to_hex((pixel[0] << 20) | (pixel[1] << 10) | pixel[2], 8) : to_hex((pixel[0] << 16) | (pixel[1] << 8) | pixel[2], 6);

  return use_10_bpc_ ? string_sprintf("(%4d,%4d,%4d#%s)", pixel[0], pixel[1], pixel[2], hex_pixel.c_str()) : string_sprintf("(%3d,%3d,%3d#%s)", pixel[0], pixel[1], pixel[2], hex_pixel.c_str());
}

std::string Display::get_and_format_rgb_yuv_pixel(uint8_t* rgb_plane, const size_t pitch, const AVFrame* frame, const int x, const int y) {
  auto rgb_format = static_cast<AVPixelFormat>(frame->format);

  const std::array<int, 3> rgb = get_rgb_pixel(rgb_plane, pitch, x, y);
  const std::array<int, 3> yuv = convert_rgb_to_yuv(rgb, rgb_format, frame->colorspace, frame->color_range);

  return "RGB" + format_pixel(rgb) + ", YUV" + format_pixel(yuv);
}

float* Display::rgb_to_grayscale(const uint8_t* plane, const size_t pitch) {
  float* grayscale_image = new float[video_width_ * video_height_];
  float* p_out = grayscale_image;

  auto to_grayscale = [](const float r, const float g, const float b, const float normalization_factor) -> float { return (r * 0.299f + g * 0.587f + b * 0.114f) * normalization_factor; };

  if (use_10_bpc_) {
    const uint16_t* p_in = reinterpret_cast<const uint16_t*>(plane);

    for (int y = 0; y < video_height_; y++) {
      for (int x = 0; x < (video_width_ * 3); x += 3) {
        const float r = p_in[x] >> 6;
        const float g = p_in[x + 1] >> 6;
        const float b = p_in[x + 2] >> 6;

        *(p_out++) = to_grayscale(r, g, b, 1.f / 1023.f);
      }

      p_in += pitch / sizeof(uint16_t);
    }
  } else {
    for (int y = 0; y < video_height_; y++) {
      for (int x = 0; x < (video_width_ * 3); x += 3) {
        const float r = plane[x];
        const float g = plane[x + 1];
        const float b = plane[x + 2];

        *(p_out++) = to_grayscale(r, g, b, 1.f / 255.f);
      }

      plane += pitch;
    }
  }

  return grayscale_image;
}

float Display::compute_ssim_block(const float* left_plane, const float* right_plane, const int x_offset, const int y_offset, const int block_size) {
  const int block_elements = block_size * block_size;

  auto compute_mean = [&](const float* plane) {
    float sum = 0;

    for (int y = y_offset; y < (y_offset + block_size); y++) {
      const float* row = plane + y * video_width_ + x_offset;

      for (int x = 0; x < block_size; x++) {
        sum += *(row++);
      }
    }

    return sum / block_elements;
  };

  float mean1 = compute_mean(left_plane);
  float mean2 = compute_mean(right_plane);

  // compute variance and convariance
  float sum_var1 = 0, sum_var2 = 0, sum_covar = 0;

  for (int y = y_offset; y < (y_offset + block_size); y++) {
    const float* row1 = left_plane + y * video_width_ + x_offset;
    const float* row2 = right_plane + y * video_width_ + x_offset;

    for (int x = 0; x < block_size; x++) {
      float diff1 = *(row1++) - mean1;
      float diff2 = *(row2++) - mean2;

      sum_var1 += diff1 * diff1;
      sum_var2 += diff2 * diff2;
      sum_covar += diff1 * diff2;
    }
  }

  float variance1 = sum_var1 / block_elements;
  float variance2 = sum_var2 / block_elements;
  float covariance = sum_covar / block_elements;

  float geomtric_mean_variance12 = sqrtf(variance1 * variance2);

  // compute SSIM metrics
  static constexpr float k1 = 0.01f;
  static constexpr float k2 = 0.03f;
  static constexpr float c1 = k1 * k1;
  static constexpr float c2 = k2 * k2;
  static constexpr float c3 = c2 / 2.f;

  float luminance = (2.f * mean1 * mean2 + c1) / (mean1 * mean1 + mean2 * mean2 + c1);
  float contrast = (2.f * geomtric_mean_variance12 + c2) / (variance1 + variance2 + c2);
  float structure = (covariance + c3) / (geomtric_mean_variance12 + c3);

  return luminance * contrast * structure;
}

float Display::compute_ssim(const float* left_plane, const float* right_plane) {
  static constexpr int overlap = 4;
  static constexpr int block_size = 8;

  float ssim_sum = 0.0;
  int count = 0;

  for (int y = 0; y < video_height_ - (block_size - 1); y += block_size - overlap) {
    for (int x = 0; x < video_width_ - (block_size - 1); count++, x += block_size - overlap) {
      ssim_sum += compute_ssim_block(left_plane, right_plane, x, y, block_size);
    }
  }

  return ssim_sum / count;
}

float Display::compute_psnr(const float* left_plane, const float* right_plane) {
  // compute MSE
  float mse = 0.0;

  for (int i = 0; i < (video_width_ * video_height_); i++) {
    const float diff = *(left_plane++) - *(right_plane++);

    mse += diff * diff;
  }

  mse /= (video_width_ * video_height_);

  if (mse == 0) {
    return std::numeric_limits<float>::infinity();
  }

  // compute PSNR
  return -10.f * log10f(mse);
}

void Display::render_help() {
  SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA * 3 / 2);
  SDL_RenderFillRect(renderer_, nullptr);

  int y = help_y_offset_;

  for (size_t i = 0; i < help_textures_.size(); i++) {
    int w, h;
    SDL_QueryTexture(help_textures_[i], nullptr, nullptr, &w, &h);

    SDL_Rect screen_area = {HELP_TEXT_HORIZONTAL_MARGIN, y, w, h};
    SDL_RenderCopy(renderer_, help_textures_[i], nullptr, &screen_area);

    y += h + HELP_TEXT_LINE_SPACING;
  }
}

void Display::render_metadata_overlay() {
  // Check if swap state has changed and refresh metadata if needed
  if (swap_left_right_ != last_swap_left_right_state_) {
    last_swap_left_right_state_ = swap_left_right_;
    update_metadata(right_metadata_, left_metadata_);
  }

  SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA * 3 / 2);
  SDL_RenderFillRect(renderer_, nullptr);

  const int table_width = drawable_width_ - HELP_TEXT_HORIZONTAL_MARGIN * 2;
  const int table_x = HELP_TEXT_HORIZONTAL_MARGIN;

  // Calculate the starting Y position to center the table vertically
  int y;

  if (mode_ == Mode::VSTACK && metadata_total_height_ < drawable_height_ / 2) {
    y = (drawable_height_ / 2 - metadata_total_height_) / 2;
  } else if (mode_ != Mode::VSTACK && metadata_total_height_ < drawable_height_) {
    y = (drawable_height_ - metadata_total_height_) / 2;
  } else {
    y = metadata_y_offset_ + 10;
  }

  for (size_t i = 0; i < metadata_textures_.size(); i++) {
    int w, h;
    SDL_QueryTexture(metadata_textures_[i], nullptr, nullptr, &w, &h);

    int x_offset = (table_width - w) / 2;

    SDL_Rect screen_area = {table_x + x_offset, y, w, h};
    SDL_RenderCopy(renderer_, metadata_textures_[i], nullptr, &screen_area);

    y += h + HELP_TEXT_LINE_SPACING;
  }
}

void Display::update_metadata(const VideoMetadata left_metadata, const VideoMetadata right_metadata) {
  // Store the metadata for later use when swapping
  left_metadata_ = left_metadata;
  right_metadata_ = right_metadata;

  constexpr char TOKENIZER = ',';

  for (auto texture : metadata_textures_) {
    SDL_DestroyTexture(texture);
  }
  metadata_textures_.clear();
  metadata_total_height_ = 0;

  auto add_metadata_texture = [&](TTF_Font* font, const std::string& text, bool primary_color, bool is_header) {
    int h;

    // choose text color based on content type and alternating pattern
    SDL_Color text_color = is_header ? HELP_TEXT_PRIMARY_COLOR : (primary_color ? HELP_TEXT_PRIMARY_COLOR : HELP_TEXT_ALTERNATE_COLOR);

    // render text with word wrapping to fit available width
    SDL_Surface* surface = TTF_RenderUTF8_Blended_Wrapped(font, text.c_str(), text_color, drawable_width_ - HELP_TEXT_HORIZONTAL_MARGIN * 2);
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_FreeSurface(surface);

    // get texture dimensions and accumulate total height for scrolling calculations
    SDL_QueryTexture(texture, nullptr, nullptr, nullptr, &h);
    metadata_total_height_ += h + HELP_TEXT_LINE_SPACING;

    metadata_textures_.push_back(texture);
  };

  // Calculate max length for column sizing
  auto calculate_max_length = [](const VideoMetadata& metadata) -> size_t {
    size_t max_length = 0;
    for (const auto& kv : metadata.properties) {
      // comma tokenize value and find max length among all tokens
      std::vector<std::string> tokens = string_split(kv.second, TOKENIZER);
      for (const auto& token : tokens) {
        max_length = std::max(max_length, token.length());
      }
    }
    return max_length;
  };

  auto left_max_length = calculate_max_length(left_metadata);
  auto right_max_length = calculate_max_length(right_metadata);

  const std::vector<std::string> properties(MetadataProperties::ALL, MetadataProperties::ALL + MetadataProperties::COUNT);

  // calculate available display width (accounting for margins)
  const int available_width = drawable_width_ - HELP_TEXT_HORIZONTAL_MARGIN * 2;

  // dynamic column width calculation
  constexpr int spacing = 2;

  // calculate initial column widths based on content
  int prop_cols = MetadataProperties::LONGEST + spacing;
  int left_cols = left_max_length + spacing;
  int right_cols = right_max_length + spacing;
  int total_cols = prop_cols + left_cols + right_cols;

  // determine character widths for both font sizes to choose optimal font
  const std::string test_text = "FOR COMPUTING THE AVERAGE CHARACTER WIDTHS, WE NEED TO TEST THE WIDTH OF A STRING";

  int char_width_small = 10;
  int char_width_big = 14;

  int text_width, text_height;

  if (TTF_SizeText(small_font_, test_text.c_str(), &text_width, &text_height) == 0) {
    char_width_small = text_width / test_text.length() + 1;
  }
  if (TTF_SizeText(big_font_, test_text.c_str(), &text_width, &text_height) == 0) {
    char_width_big = text_width / test_text.length() + 1;
  }

  // calculate how many characters can fit per line with each font
  const int max_cols_per_line_big = available_width / char_width_big;
  const int max_cols_per_line_small = available_width / char_width_small;

  // choose the largest font that can accommodate all columns
  const int char_width = max_cols_per_line_big >= total_cols ? char_width_big : char_width_small;
  auto font = max_cols_per_line_big >= total_cols ? big_font_ : small_font_;

  const int max_cols_per_line = available_width / char_width;

  // if content is too wide for the window, proportionally reduce column widths
  if (total_cols > max_cols_per_line) {
    const int overshoot = total_cols - max_cols_per_line;

    // distribute the overshoot proportionally across columns
    // property column gets priority (2x weight) since it's the least important
    const int prop_cols_overshoot = std::min(prop_cols, overshoot * prop_cols / total_cols * 2);
    const int left_cols_overshoot = std::max(0, overshoot - prop_cols_overshoot) * left_cols / (left_cols + right_cols);
    const int right_cols_overshoot = overshoot - prop_cols_overshoot - left_cols_overshoot;

    prop_cols -= prop_cols_overshoot;
    left_cols -= left_cols_overshoot;
    right_cols -= right_cols_overshoot;
  }

  // generate table header
  TTF_SetFontStyle(font, TTF_STYLE_ITALIC | TTF_STYLE_UNDERLINE);
  add_metadata_texture(font, string_sprintf("%-*s%-*s%-*s", prop_cols, "", left_cols, "LEFT", right_cols, "RIGHT"), true, false);
  TTF_SetFontStyle(font, TTF_STYLE_NORMAL);

  bool primary_color = false;

  for (const auto& prop : properties) {
    std::string prop_value = to_upper_case(prop);

    // extract values for both videos
    std::string left_value = left_metadata.get(prop);
    std::string right_value = right_metadata.get(prop);

    // tokenize values by comma
    std::vector<std::string> left_tokens = string_split(left_value, TOKENIZER);
    std::vector<std::string> right_tokens = string_split(right_value, TOKENIZER);

    // determine how many lines we need for this property
    size_t max_tokens = std::max(left_tokens.size(), right_tokens.size());

    for (size_t i = 0; i < max_tokens; i++) {
      std::string current_prop_value = (i == 0) ? prop_value : "";
      std::string current_left_value = (i < left_tokens.size()) ? left_tokens[i] : "";
      std::string current_right_value = (i < right_tokens.size()) ? right_tokens[i] : "";

      // text truncation for narrow columns
      if (static_cast<int>(current_prop_value.length()) >= prop_cols) {
        current_prop_value = prop_cols > 1 ? current_prop_value.substr(0, prop_cols - 2) + "… " : "";
      }
      if (static_cast<int>(current_left_value.length()) >= left_cols) {
        current_left_value = "…" + current_left_value.substr(current_left_value.length() - left_cols + 2) + " ";
      }
      if (static_cast<int>(current_right_value.length()) >= right_cols) {
        current_right_value = "…" + current_right_value.substr(current_right_value.length() - right_cols + 2) + " ";
      }

      add_metadata_texture(font, string_sprintf("%-*s%-*s%-*s", prop_cols, current_prop_value.c_str(), left_cols, current_left_value.c_str(), right_cols, current_right_value.c_str()), primary_color, false);

      primary_color = !primary_color;
    }
  }
}

SDL_Rect Display::get_left_selection_rect() const {
  const int x = std::min(selection_start_.x(), selection_end_.x());
  const int y = std::min(selection_start_.y(), selection_end_.y());
  const int w = std::abs(selection_end_.x() - selection_start_.x());
  const int h = std::abs(selection_end_.y() - selection_start_.y());

  const int clipped_x = std::max(0, x);
  const int clipped_y = std::max(0, y);
  const int clipped_w = std::min(w - (clipped_x - x), video_width_ - clipped_x);
  const int clipped_h = std::min(h - (clipped_y - y), video_height_ - clipped_y);

  return {clipped_x, clipped_y, clipped_w, clipped_h};
}

void Display::draw_selection_rect() {
  if (selection_state_ != SelectionState::STARTED) {
    return;
  }

  const auto zoom_rect = compute_zoom_rect();

  auto draw_rect = [this](const SDL_FRect& r, Uint8 r_val, Uint8 g_val, Uint8 b_val) {
    // Draw semi-transparent overlay
    SDL_SetRenderDrawColor(renderer_, r_val / 2, g_val / 2, b_val / 2, 128);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_RenderFillRectF(renderer_, &r);

    // Draw border
    SDL_SetRenderDrawColor(renderer_, r_val, g_val, b_val, 255);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_RenderDrawRectF(renderer_, &r);
  };

  SDL_Rect selection_rect = get_left_selection_rect();
  SDL_FRect drawable_rect = video_rect_to_drawable_transform(video_to_zoom_space(selection_rect, zoom_rect));

  if (mode_ == Mode::SPLIT) {
    // For split mode, we don't need to draw a second rectangle
    draw_rect(drawable_rect, 255, 255, 255);
    return;
  } else {
    draw_rect(drawable_rect, 255, 128, 128);
  }

  // Draw right rectangle with appropriate offset
  switch (mode_) {
    case Mode::HSTACK:
      selection_rect.x += video_width_;
      break;
    case Mode::VSTACK:
      selection_rect.y += video_height_;
      break;
    default:
      break;
  }

  drawable_rect = video_rect_to_drawable_transform(video_to_zoom_space(selection_rect, zoom_rect));
  draw_rect(drawable_rect, 128, 128, 255);
}

void Display::possibly_save_selected_area(const AVFrame* left_frame, const AVFrame* right_frame) {
  if (selection_state_ != SelectionState::COMPLETED) {
    return;
  }

  const SDL_Rect selection_rect = get_left_selection_rect();

  if (selection_rect.w <= 0 || selection_rect.h <= 0) {
    std::cerr << "Selection rectangle is empty. Please make a valid selection." << std::endl;
  } else {
    save_selected_area(left_frame, right_frame, selection_rect);
  }

  selection_state_ = SelectionState::NONE;
  save_selected_area_ = false;
}

void Display::save_selected_area(const AVFrame* left_frame, const AVFrame* right_frame, const SDL_Rect& selection_rect) {
  std::atomic_bool error_occurred(false);

  // Lambda for creating and initializing frames
  auto create_frame = [&](const int width, const int height, const AVFrame* source_frame) -> AVFrame* {
    AVFrame* frame = av_frame_alloc();
    frame->format = source_frame->format;
    frame->width = width;
    frame->height = height;
    frame->colorspace = source_frame->colorspace;
    frame->color_range = source_frame->color_range;
    av_frame_get_buffer(frame, 0);
    return frame;
  };

  AVFrame* left_selected = create_frame(selection_rect.w, selection_rect.h, left_frame);
  AVFrame* right_selected = create_frame(selection_rect.w, selection_rect.h, right_frame);
  AVFrame* concatenated = create_frame(selection_rect.w * 2, selection_rect.h, left_frame);

  const int pixel_size = use_10_bpc_ ? 3 * sizeof(uint16_t) : 3;

  for (int y = 0; y < selection_rect.h; y++) {
    const int src_y = selection_rect.y + y;
    const int dst_y = y;

    // Copy left frame data
    memcpy(left_selected->data[0] + dst_y * left_selected->linesize[0], left_frame->data[0] + src_y * left_frame->linesize[0] + selection_rect.x * pixel_size, selection_rect.w * pixel_size);

    // Copy right frame data
    memcpy(right_selected->data[0] + dst_y * right_selected->linesize[0], right_frame->data[0] + src_y * right_frame->linesize[0] + selection_rect.x * pixel_size, selection_rect.w * pixel_size);

    // Copy to concatenated frame
    memcpy(concatenated->data[0] + dst_y * concatenated->linesize[0], left_frame->data[0] + src_y * left_frame->linesize[0] + selection_rect.x * pixel_size, selection_rect.w * pixel_size);
    memcpy(concatenated->data[0] + dst_y * concatenated->linesize[0] + selection_rect.w * pixel_size, right_frame->data[0] + src_y * right_frame->linesize[0] + selection_rect.x * pixel_size, selection_rect.w * pixel_size);
  }

  const std::string left_filename = string_sprintf("%s%s_cutout_%04d.png", left_file_stem_.c_str(), (left_file_stem_ == right_file_stem_) ? "_left" : "", saved_selected_image_number_);
  const std::string right_filename = string_sprintf("%s%s_cutout_%04d.png", right_file_stem_.c_str(), (left_file_stem_ == right_file_stem_) ? "_right" : "", saved_selected_image_number_);
  const std::string concatenated_filename = string_sprintf("%s_%s_cutout_concat_%04d.png", left_file_stem_.c_str(), right_file_stem_.c_str(), saved_selected_image_number_);

  auto save_frame = [&](const AVFrame* frame, const std::string& filename) { return write_png(frame, filename, error_occurred); };

  std::thread save_left_thread(save_frame, left_selected, left_filename);
  std::thread save_right_thread(save_frame, right_selected, right_filename);
  std::thread save_concatenated_thread(save_frame, concatenated, concatenated_filename);

  save_left_thread.join();
  save_right_thread.join();
  save_concatenated_thread.join();

  av_frame_free(&left_selected);
  av_frame_free(&right_selected);
  av_frame_free(&concatenated);

  if (!error_occurred) {
    std::cout << "Saved " << string_sprintf("%s, %s and %s", left_filename.c_str(), right_filename.c_str(), concatenated_filename.c_str()) << std::endl;

    saved_selected_image_number_++;
  }
}

bool Display::possibly_refresh(const AVFrame* left_frame, const AVFrame* right_frame, const std::string& current_total_browsable, const std::string& message) {
  const bool has_updated_left_pts = previous_left_frame_pts_ != left_frame->pts;
  const bool has_updated_right_pts = previous_right_frame_pts_ != right_frame->pts;

  if (!input_received_ && !has_updated_left_pts && !has_updated_right_pts && !timer_based_update_performed_ && message.empty()) {
    return false;
  }

  std::array<uint8_t*, 3> planes_left{left_frame->data[0], left_frame->data[1], left_frame->data[2]};
  std::array<uint8_t*, 3> planes_right{right_frame->data[0], right_frame->data[1], right_frame->data[2]};
  std::array<size_t, 3> pitches_left{static_cast<size_t>(left_frame->linesize[0]), static_cast<size_t>(left_frame->linesize[1]), static_cast<size_t>(left_frame->linesize[2])};
  std::array<size_t, 3> pitches_right{static_cast<size_t>(right_frame->linesize[0]), static_cast<size_t>(right_frame->linesize[1]), static_cast<size_t>(right_frame->linesize[2])};

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

  const bool compare_mode = show_left_ && show_right_;

  const auto zoom_rect = compute_zoom_rect();

  const Vector2D mouse_video_pos = get_mouse_video_position(mouse_x_, mouse_y_, zoom_rect);
  const int mouse_video_x = mouse_video_pos.x();
  const int mouse_video_y = mouse_video_pos.y();

  // print pixel position in original video coordinates and RGB+YUV color value
  if (print_mouse_position_and_color_) {
    const bool print_left_pixel = mouse_video_x >= 0 && mouse_video_x < video_width_ && mouse_video_y >= 0 && mouse_video_y < video_height_;

    bool print_right_pixel;

    switch (mode_) {
      case Mode::HSTACK:
        print_right_pixel = mouse_video_x >= video_width_ && mouse_video_x < (2 * video_width_) && mouse_video_y >= 0 && mouse_video_y < video_height_;
        break;
      case Mode::VSTACK:
        print_right_pixel = mouse_video_x >= 0 && mouse_video_x < video_width_ && mouse_video_y >= video_height_ && mouse_video_y < (video_height_ * 2);
        break;
      default:
        print_right_pixel = print_left_pixel;
    }

    if (print_left_pixel || print_right_pixel) {
      const int pixel_video_x = mouse_video_x % video_width_;
      const int pixel_video_y = mouse_video_y % video_height_;

      auto get_original_dimensions = [&](const AVFrame* frame) -> std::pair<int, int> {
        const int original_width = get_metadata_int_value(frame, "original_width", frame->width);
        const int original_height = get_metadata_int_value(frame, "original_height", frame->height);

        return std::make_pair(original_width, original_height);
      };

      auto original_left_dims = get_original_dimensions(left_frame);
      auto original_right_dims = get_original_dimensions(right_frame);

      std::cout << "Left:  " << string_sprintf("[%4d,%4d]", pixel_video_x * original_left_dims.first / video_width_, pixel_video_y * original_left_dims.second / video_height_);
      std::cout << ", " << get_and_format_rgb_yuv_pixel(planes_left[0], pitches_left[0], left_frame, pixel_video_x, pixel_video_y);
      std::cout << " - ";
      std::cout << "Right: " << string_sprintf("[%4d,%4d]", pixel_video_x * original_right_dims.first / video_width_, pixel_video_y * original_right_dims.second / video_height_);
      std::cout << ", " << get_and_format_rgb_yuv_pixel(planes_right[0], pitches_right[0], right_frame, pixel_video_x, pixel_video_y);
      std::cout << std::endl;
    }

    print_mouse_position_and_color_ = false;
  }

  // print image similarity metrics
  if (print_image_similarity_metrics_) {
    const float* left_gray = rgb_to_grayscale(planes_left[0], pitches_left[0]);
    const float* right_gray = rgb_to_grayscale(planes_right[0], pitches_right[0]);

    std::cout << string_sprintf("Metrics: [%s|%s], PSNR(%.3f), SSIM(%.5f), VMAF(%s)", format_position(ffmpeg::pts_in_secs(left_frame), false).c_str(), format_position(ffmpeg::pts_in_secs(right_frame), false).c_str(),
                                compute_psnr(left_gray, right_gray), compute_ssim(left_gray, right_gray), VMAFCalculator::instance().compute(left_frame, right_frame).c_str())
              << std::endl;

    delete left_gray;
    delete right_gray;

    print_image_similarity_metrics_ = false;
  }

  // clear everything
  SDL_SetRenderDrawColor(renderer_, BACKGROUND_COLOR.r, BACKGROUND_COLOR.g, BACKGROUND_COLOR.b, BACKGROUND_COLOR.a);
  SDL_RenderClear(renderer_);

  // mouse video x-position stretched to full window extent
  const float full_ws_mouse_video_x = static_cast<float>(mouse_x_ * window_width_ / (window_width_ - 1)) * video_to_window_width_factor_;

  // mouse x-position in video coordinates
  const float video_mouse_x = (full_ws_mouse_video_x - zoom_rect.start.x()) * static_cast<float>(video_width_) / zoom_rect.size.x();

  // the nearest texel border to the mouse x-position in window coordinates
  const float video_texel_clamped_mouse_x = (std::round(video_mouse_x) * zoom_rect.size.x() / static_cast<float>(video_width_) + zoom_rect.start.x()) / video_to_window_width_factor_;

  if (show_left_ || show_right_) {
    const int split_x = (compare_mode && mode_ == Mode::SPLIT) ? clamp_range(std::round(video_mouse_x), 0.0F, float(video_width_)) : show_left_ ? video_width_ : 0;

    // update video
    if (show_left_ && (split_x > 0)) {
      const SDL_Rect tex_render_quad_left = {0, 0, split_x, video_height_};
      const SDL_FRect screen_render_quad_left = video_rect_to_drawable_transform(video_to_zoom_space(tex_render_quad_left, zoom_rect));

      if (input_received_ || has_updated_left_pts) {
        if (use_10_bpc_) {
          convert_to_packed_10_bpc(planes_left, pitches_left, left_planes_, pitches_left, tex_render_quad_left);

          update_texture(&tex_render_quad_left, left_planes_[0], pitches_left[0], "left update (10 bpc, video mode)");
        } else {
          update_texture(&tex_render_quad_left, planes_left[0], pitches_left[0], "left update (video mode)");
        }
      }

      check_sdl(SDL_RenderCopyF(renderer_, get_video_texture(), &tex_render_quad_left, &screen_render_quad_left) == 0, "left video texture render copy");
    }
    if (show_right_ && ((split_x < video_width_) || mode_ != Mode::SPLIT)) {
      const int start_right = (mode_ == Mode::SPLIT) ? std::max(split_x, 0) : 0;
      const int right_x_offset = (mode_ == Mode::HSTACK) ? video_width_ : 0;
      const int right_y_offset = (mode_ == Mode::VSTACK) ? video_height_ : 0;

      const SDL_Rect tex_render_quad_right = {right_x_offset + start_right, right_y_offset, (video_width_ - start_right), video_height_};
      const SDL_Rect roi = {start_right, 0, (video_width_ - start_right), video_height_};
      const SDL_FRect screen_render_quad_right = video_rect_to_drawable_transform(video_to_zoom_space(tex_render_quad_right, zoom_rect));

      if (input_received_ || has_updated_right_pts) {
        if (subtraction_mode_) {
          update_difference(planes_left, pitches_left, planes_right, pitches_right, start_right);

          if (use_10_bpc_) {
            convert_to_packed_10_bpc(diff_planes_, diff_pitches_, right_planes_, pitches_right, roi);

            update_texture(&tex_render_quad_right, right_planes_[0] + start_right, pitches_right[0], "right update (10 bpc, subtraction mode)");
          } else {
            update_texture(&tex_render_quad_right, diff_planes_[0] + start_right * 3, diff_pitches_[0], "right update (subtraction mode)");
          }
        } else {
          if (use_10_bpc_) {
            convert_to_packed_10_bpc(planes_right, pitches_right, right_planes_, pitches_right, roi);

            update_texture(&tex_render_quad_right, right_planes_[0] + start_right, pitches_right[0], "right update (10 bpc, video mode)");
          } else {
            update_texture(&tex_render_quad_right, planes_right[0] + start_right * 3, pitches_right[0], "right update (video mode)");
          }
        }
      }

      check_sdl(SDL_RenderCopyF(renderer_, get_video_texture(), &tex_render_quad_right, &screen_render_quad_right) == 0, "right video texture render copy");
    }
  }

  const int mouse_drawable_x = std::round(video_texel_clamped_mouse_x * drawable_to_window_width_factor_);
  const int mouse_drawable_y = std::round(static_cast<float>(mouse_y_) * drawable_to_window_height_factor_);

  // zoomed area
  const int dst_zoomed_size = static_cast<int>(std::round(std::min(drawable_width_, drawable_height_) * 0.5F)) & -2;  // size must be an even number of pixels
  const int dst_half_zoomed_size = dst_zoomed_size / 2;

  if (zoom_left_ || zoom_right_) {
    const int src_zoomed_size = 64;
    const int src_half_zoomed_size = src_zoomed_size / 2;

    SDL_Rect src_zoomed_area = {clamp_range(mouse_drawable_x - src_half_zoomed_size, 0, drawable_width_ - src_zoomed_size - 1), clamp_range(mouse_drawable_y - src_half_zoomed_size, 0, drawable_height_ - src_zoomed_size - 1),
                                src_zoomed_size, src_zoomed_size};

    SDL_Surface* render_surface = SDL_CreateRGBSurface(0, src_zoomed_size, src_zoomed_size, 32, 0, 0, 0, 0);
    SDL_RenderReadPixels(renderer_, &src_zoomed_area, render_surface->format->format, render_surface->pixels, render_surface->pitch);
    SDL_Texture* render_texture = SDL_CreateTextureFromSurface(renderer_, render_surface);

    if (zoom_left_) {
      const SDL_Rect dst_zoomed_area = {0, drawable_height_ - dst_zoomed_size, dst_zoomed_size, dst_zoomed_size};
      SDL_RenderCopy(renderer_, render_texture, nullptr, &dst_zoomed_area);
    }
    if (zoom_right_) {
      const SDL_Rect dst_zoomed_area = {drawable_width_ - dst_zoomed_size, drawable_height_ - dst_zoomed_size, dst_zoomed_size, dst_zoomed_size};
      SDL_RenderCopy(renderer_, render_texture, nullptr, &dst_zoomed_area);
    }

    SDL_DestroyTexture(render_texture);
    SDL_FreeSurface(render_surface);
  }

  timer_based_update_performed_ = false;

  SDL_Rect fill_rect;
  SDL_Rect text_rect;
  SDL_Surface* text_surface;

  if (show_hud_) {
    const float left_position = ffmpeg::pts_in_secs(left_frame);
    const float right_position = ffmpeg::pts_in_secs(right_frame);
    const float left_progress = left_position + ffmpeg::frame_duration_in_secs(left_frame);
    const float right_progress = right_position + ffmpeg::frame_duration_in_secs(right_frame);

    // render background rectangles and text on top
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    if (show_left_) {
      // file name and current position of left video
      const std::string left_picture_type(1, av_get_picture_type_char(left_frame->pict_type));
      const std::string left_pos_str = format_position(left_position, true) + " " + left_picture_type + format_position_difference(left_position, right_position);
      text_surface = TTF_RenderText_Blended(small_font_, left_pos_str.c_str(), POSITION_COLOR);
      SDL_Texture* left_position_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
      const int left_position_text_width = text_surface->w;
      const int left_position_text_height = text_surface->h;
      SDL_FreeSurface(text_surface);

      if (mode_ == Mode::VSTACK) {
        render_text(line1_y_, line1_y_, left_position_text_texture, left_position_text_width, left_position_text_height, border_extension_, true);
        render_text(line1_y_, line2_y_, left_text_texture_, left_text_width_, left_text_height_, border_extension_, true);
      } else {
        render_text(line1_y_, line1_y_, left_text_texture_, left_text_width_, left_text_height_, border_extension_, true);
        render_text(line1_y_, line2_y_, left_position_text_texture, left_position_text_width, left_position_text_height, border_extension_, true);
      }

      SDL_DestroyTexture(left_position_text_texture);
    }
    if (show_right_) {
      // file name and current position of right video
      const std::string right_picture_type(1, av_get_picture_type_char(right_frame->pict_type));
      const std::string right_pos_str = format_position(right_position, true) + " " + right_picture_type + format_position_difference(right_position, left_position);
      text_surface = TTF_RenderText_Blended(small_font_, right_pos_str.c_str(), POSITION_COLOR);
      SDL_Texture* right_position_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
      int right_position_text_width = text_surface->w;
      int right_position_text_height = text_surface->h;
      SDL_FreeSurface(text_surface);

      int text1_x;
      int text1_y;
      int text2_x;
      int text2_y;

      if (mode_ == Mode::VSTACK) {
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
    if (mouse_is_inside_window_ && duration_ > 0) {
      // target seek position
      float target_position = static_cast<float>(mouse_x_) / static_cast<float>(window_width_) * duration_;

      const std::string target_pos_str = format_position(target_position, true);
      text_surface = TTF_RenderText_Blended(small_font_, target_pos_str.c_str(), TARGET_COLOR);
      SDL_Texture* target_position_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
      const int target_position_text_width = text_surface->w;
      const int target_position_text_height = text_surface->h;
      SDL_FreeSurface(text_surface);

      SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA * 2);
      render_text(drawable_width_ - line1_y_ - target_position_text_width, drawable_height_ - line1_y_ - target_position_text_height, target_position_text_texture, target_position_text_width, target_position_text_height, border_extension_,
                  false);

      SDL_DestroyTexture(target_position_text_texture);
    }

    // zoom factor
    std::string zoom_factor_str;
    const uint64_t global_zoom_factor_rounded = lrintf(global_zoom_factor_ * 1000);
    int global_zoom_factor_trailing_zeros = (global_zoom_factor_rounded % 10) > 0 ? 0 : 1;
    global_zoom_factor_trailing_zeros += (global_zoom_factor_rounded % 100) > 0 ? 0 : 1;
    global_zoom_factor_trailing_zeros += (global_zoom_factor_rounded % 1000) > 0 ? 0 : 1;

    if (global_zoom_factor_ < 1e-1 || (global_zoom_factor_trailing_zeros == 0 && global_zoom_factor_rounded < 1000)) {
      zoom_factor_str = string_sprintf("x%1.3f", global_zoom_factor_);
    } else if (global_zoom_factor_trailing_zeros <= 1 && global_zoom_factor_rounded < 10000) {
      zoom_factor_str = string_sprintf("x%1.2f", global_zoom_factor_);
    } else if (global_zoom_factor_trailing_zeros <= 2 && global_zoom_factor_rounded < 100000) {
      zoom_factor_str = string_sprintf("x%1.1f", global_zoom_factor_);
    } else {
      zoom_factor_str = string_sprintf("x%1.0f", global_zoom_factor_);
    }

    text_surface = TTF_RenderText_Blended(small_font_, zoom_factor_str.c_str(), ZOOM_COLOR);
    SDL_Texture* zoom_position_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
    const int zoom_position_text_width = text_surface->w;
    const int zoom_position_text_height = text_surface->h;
    SDL_FreeSurface(text_surface);

    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA * 2);

    int text_x = (mode_ == Mode::VSTACK) ? drawable_width_ - line1_y_ - zoom_position_text_width : line1_y_;
    int text_y = (mode_ == Mode::VSTACK) ? line1_y_ : drawable_height_ - line1_y_ - zoom_position_text_height;

    render_text(text_x, text_y, zoom_position_text_texture, zoom_position_text_width, zoom_position_text_height, border_extension_, false);
    SDL_DestroyTexture(zoom_position_text_texture);

    // playback speed
    std::string playback_speed_str;
    std::string playback_speed_factor_str;

    const float playback_speed = 1000000.0f * playback_speed_factor_ / float(std::max(ffmpeg::frame_duration(left_frame), ffmpeg::frame_duration(right_frame)));
    const uint64_t playback_speed_rounded = lrintf(playback_speed * 1000);

    if (playback_speed_rounded < 1000) {
      playback_speed_str = string_sprintf("%1.2f", playback_speed);
    } else if (playback_speed_rounded % 1000 && playback_speed_rounded < 240000) {
      if (playback_speed_rounded % 100 && playback_speed_rounded < 60000) {
        playback_speed_str = string_sprintf("%1.2f", playback_speed);
      } else {
        playback_speed_str = string_sprintf("%1.1f", playback_speed);
      }
    } else {
      playback_speed_str = string_sprintf("%1.0f", playback_speed);
    }

    if (playback_speed_level_ != 0) {
      if (lrintf(playback_speed_factor_ * 100) < 10) {
        playback_speed_factor_str = string_sprintf("|%1.1f%%", playback_speed_factor_ * 100);
      } else {
        playback_speed_factor_str = string_sprintf("|%1.0f%%", playback_speed_factor_ * 100);
      }
    } else {
      playback_speed_factor_str = "";
    }

    const std::string united_playback_speed_str = string_sprintf("@%s%s", playback_speed_str.c_str(), playback_speed_factor_str.c_str());
    text_surface = TTF_RenderText_Blended(small_font_, united_playback_speed_str.c_str(), PLAYBACK_SPEED_COLOR);
    SDL_Texture* playack_speed_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
    const int playack_speed_text_width = text_surface->w;
    const int playack_speed_text_height = text_surface->h;
    SDL_FreeSurface(text_surface);

    text_x = drawable_width_ / 2 - playack_speed_text_width / 2 - border_extension_;
    text_y = drawable_height_ - line1_y_ - zoom_position_text_height;

    render_text(text_x, text_y, playack_speed_text_texture, playack_speed_text_width, playack_speed_text_height, border_extension_, false);
    SDL_DestroyTexture(playack_speed_text_texture);

    // current frame / number of frames in history buffer
    text_surface = TTF_RenderText_Blended(small_font_, current_total_browsable.c_str(), BUFFER_COLOR);
    SDL_Texture* current_total_browsable_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
    const int current_total_browsable_text_width = text_surface->w;
    const int current_total_browsable_text_height = text_surface->h;
    SDL_FreeSurface(text_surface);

    text_y = (mode_ == Mode::VSTACK) ? line1_y_ : line2_y_;

    // blink label in loop mode
    fill_rect = {drawable_width_ / 2 - current_total_browsable_text_width / 2 - border_extension_, text_y - border_extension_, current_total_browsable_text_width + double_border_extension_,
                 current_total_browsable_text_height + double_border_extension_};

    SDL_Color label_color = LOOP_OFF_LABEL_COLOR;
    int label_alpha = BACKGROUND_ALPHA;

    if (buffer_play_loop_mode_ != Display::Loop::OFF) {
      label_alpha *= 1.0 + sin(float(SDL_GetTicks()) / 180.0) * 0.6;

      switch (buffer_play_loop_mode_) {
        case Display::Loop::FORWARDONLY:
          label_color = LOOP_FW_LABEL_COLOR;
          break;
        case Display::Loop::PINGPONG:
          label_color = LOOP_PP_LABEL_COLOR;
          break;
        default:
          break;
      }

      timer_based_update_performed_ = true;
    }

    SDL_SetRenderDrawColor(renderer_, label_color.r, label_color.g, label_color.b, label_alpha);
    SDL_RenderFillRect(renderer_, &fill_rect);

    text_rect = {drawable_width_ / 2 - current_total_browsable_text_width / 2, text_y, current_total_browsable_text_width, current_total_browsable_text_height};
    SDL_RenderCopy(renderer_, current_total_browsable_text_texture, nullptr, &text_rect);
    SDL_DestroyTexture(current_total_browsable_text_texture);

    // display progress as dot lines
    render_progress_dots(left_position, left_progress, true);
    render_progress_dots(right_position, right_progress, false);
  }

  // render (optional) error message
  if (!message.empty()) {
    message_shown_at_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    text_surface = TTF_RenderText_Blended(big_font_, message.c_str(), TEXT_COLOR);

    if (message_texture_ != nullptr) {
      SDL_DestroyTexture(message_texture_);
    }
    message_texture_ = SDL_CreateTextureFromSurface(renderer_, text_surface);

    message_width_ = text_surface->w;
    message_height_ = text_surface->h;
    SDL_FreeSurface(text_surface);
  }
  if (message_texture_ != nullptr) {
    std::chrono::milliseconds now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    const float keep_alpha = std::max(sqrtf(1.0F - (now - message_shown_at_).count() / 1000.0F / 4.0F), 0.0F);

    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA * keep_alpha);
    fill_rect = {drawable_width_ / 2 - message_width_ / 2 - 2, drawable_height_ / 2 - message_height_ / 2 - 2, message_width_ + 4, message_height_ + 4};
    SDL_RenderFillRect(renderer_, &fill_rect);

    SDL_SetTextureAlphaMod(message_texture_, 255 * keep_alpha);
    text_rect = {drawable_width_ / 2 - message_width_ / 2, drawable_height_ / 2 - message_height_ / 2, message_width_, message_height_};
    SDL_RenderCopy(renderer_, message_texture_, nullptr, &text_rect);

    timer_based_update_performed_ = timer_based_update_performed_ || (keep_alpha > 0.0F);
  }

  if (mode_ == Mode::SPLIT && show_hud_ && compare_mode) {
    // render movable slider(s)
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, SDL_ALPHA_OPAQUE);
    SDL_RenderDrawLine(renderer_, mouse_drawable_x, 0, mouse_drawable_x, drawable_height_);

    if (zoom_left_) {
      SDL_RenderDrawLine(renderer_, dst_half_zoomed_size, drawable_height_ - dst_zoomed_size, dst_half_zoomed_size, drawable_height_);
    }
    if (zoom_right_) {
      SDL_RenderDrawLine(renderer_, drawable_width_ - dst_half_zoomed_size - 1, drawable_height_ - dst_zoomed_size, drawable_width_ - dst_half_zoomed_size - 1, drawable_height_);
    }
  }

  draw_selection_rect();

  if (show_metadata_) {
    render_metadata_overlay();
  }

  if (show_help_) {
    render_help();
  }

  if (save_image_frames_) {
    save_image_frames(left_frame, right_frame);
    save_image_frames_ = false;
  }

  if (save_selected_area_) {
    possibly_save_selected_area(left_frame, right_frame);
  }

  SDL_RenderPresent(renderer_);

  input_received_ = false;
  previous_left_frame_pts_ = left_frame->pts;
  previous_right_frame_pts_ = right_frame->pts;

  return true;
}

float Display::compute_zoom_factor(const float zoom_level) const {
  return pow(ZOOM_STEP_SIZE, zoom_level);
}

Vector2D Display::compute_relative_move_offset(const Vector2D& zoom_point, const float zoom_factor) const {
  const float zoom_factor_change = zoom_factor / global_zoom_factor_;

  const Vector2D view_center(static_cast<float>(window_width_) / (mode_ == Mode::HSTACK ? 4.0F : 2.0F) * video_to_window_width_factor_,
                             static_cast<float>(window_height_) / (mode_ == Mode::VSTACK ? 4.0F : 2.0F) * video_to_window_height_factor_);

  // the center point has to be moved relative to the zoom point
  const Vector2D new_move_offset = move_offset_ - (view_center + move_offset_ - zoom_point) * (1.0F - zoom_factor_change);

  return new_move_offset;
}

void Display::update_zoom_factor_and_move_offset(const float zoom_factor) {
  const Vector2D zoom_point(static_cast<float>(video_width_) * (mode_ == Mode::HSTACK ? 1.0F : 0.5F), static_cast<float>(video_height_) * (mode_ == Mode::VSTACK ? 1.0F : 0.5F));
  update_move_offset(compute_relative_move_offset(zoom_point, zoom_factor));

  update_zoom_factor(zoom_factor);
}

void Display::update_zoom_factor(const float zoom_factor) {
  global_zoom_factor_ = zoom_factor;
  global_zoom_level_ = log(zoom_factor) / log(ZOOM_STEP_SIZE);
}

void Display::update_move_offset(const Vector2D& move_offset) {
  move_offset_ = move_offset;
  global_center_ = Vector2D(move_offset_.x() / video_width_ + 0.5F, move_offset_.y() / video_height_ + 0.5F);
}

Display::ZoomRect Display::compute_zoom_rect() const {
  const Vector2D video_extent(video_width_, video_height_);
  const Vector2D zoom_rect_start((global_center_ - global_zoom_factor_ * 0.5F) * video_extent);
  const Vector2D zoom_rect_end((global_center_ + global_zoom_factor_ * 0.5F) * video_extent);
  const Vector2D zoom_rect_size(zoom_rect_end - zoom_rect_start);
  return {zoom_rect_start, zoom_rect_end, zoom_rect_size, global_zoom_factor_};
}

Vector2D Display::get_mouse_video_position(const int mouse_x, const int mouse_y, const Display::ZoomRect& zoom_rect) const {
  const int mouse_video_x = std::floor((static_cast<float>(mouse_x) * video_to_window_width_factor_ - zoom_rect.start.x()) * static_cast<float>(video_width_) / zoom_rect.size.x());
  const int mouse_video_y = std::floor((static_cast<float>(mouse_y) * video_to_window_height_factor_ - zoom_rect.start.y()) * static_cast<float>(video_height_) / zoom_rect.size.y());

  return Vector2D(mouse_video_x, mouse_video_y);
}

SDL_FRect Display::video_to_zoom_space(const SDL_Rect& video_rect, const Display::ZoomRect& zoom_rect) {
  // transform video coordinates to the currently zoomed area space
  return SDL_FRect({zoom_rect.start.x() + float(video_rect.x) * zoom_rect.zoom_factor, zoom_rect.start.y() + float(video_rect.y) * zoom_rect.zoom_factor, std::min(float(video_rect.w) * zoom_rect.zoom_factor, zoom_rect.size.x()),
                    std::min(float(video_rect.h) * zoom_rect.zoom_factor, zoom_rect.size.y())});
};

void Display::update_playback_speed(const int playback_speed_level) {
  // allow 128x change of playback speed
  if (abs(playback_speed_level) <= (PLAYBACK_SPEED_KEY_PRESSES_TO_DOUBLE * 7)) {
    playback_speed_level_ = playback_speed_level;
    playback_speed_factor_ = pow(PLAYBACK_SPEED_STEP_SIZE, playback_speed_level);
  }
}

void Display::input() {
  seek_relative_ = 0.0F;
  seek_from_start_ = false;
  frame_buffer_offset_delta_ = 0;
  frame_navigation_delta_ = 0;
  shift_right_frames_ = 0;
  tick_playback_ = false;
  possibly_tick_playback_ = false;

  while (SDL_PollEvent(&event_) != 0) {
    input_received_ = true;
    const SDL_Keymod keymod = SDL_GetModState();
    const SDL_Keycode keycode = event_.key.keysym.sym;

    auto is_clipboard_mod_pressed = [keymod]() -> bool {
#ifdef __APPLE__
      return (keymod & KMOD_GUI);
#else
      return (keymod & KMOD_CTRL);
#endif
    };

    auto update_cursor = [&]() {
      SDL_Cursor* cursor;

      if (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_RMASK) {
        cursor = pan_mode_cursor_;
      } else if (save_selected_area_ && selection_state_ != SelectionState::COMPLETED) {
        cursor = selection_mode_cursor_;
      } else {
        cursor = normal_mode_cursor_;
      }

      SDL_SetCursor(cursor);
    };

    auto wrap_to_left_frame = [&](Vector2D& video_position) -> Vector2D {
      switch (mode_) {
        case Mode::HSTACK:
          return video_position - Vector2D(video_width_, 0);
        case Mode::VSTACK:
          return video_position - Vector2D(0, video_height_);
        default:
          break;
      }

      return video_position;
    };

    auto handle_scroll = [&](int& y_offset, const int total_height, std::vector<SDL_Texture*>& textures) {
      y_offset += (-event_.motion.yrel * total_height * 3) / drawable_height_;
      y_offset = std::max(y_offset, drawable_height_ - total_height - static_cast<int>(textures.size()) * HELP_TEXT_LINE_SPACING);
      y_offset = std::min(y_offset, 0);
    };

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
      case SDL_MOUSEWHEEL:
        if (mouse_is_inside_window_ && event_.wheel.y != 0) {
          float delta_zoom = wheel_sensitivity_ * event_.wheel.y * (event_.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1);

          if (delta_zoom > 0) {
            delta_zoom /= 2.0F;
          }

          const float new_global_zoom_factor = compute_zoom_factor(global_zoom_level_ - delta_zoom);

          // logic ported from YUView's MoveAndZoomableView.cpp with thanks :)
          if (new_global_zoom_factor >= 0.001 && new_global_zoom_factor <= 10000) {
            const Vector2D zoom_point = Vector2D(static_cast<float>(mouse_x_) * video_to_window_width_factor_, static_cast<float>(mouse_y_) * video_to_window_height_factor_);
            update_move_offset(compute_relative_move_offset(zoom_point, new_global_zoom_factor));

            global_zoom_level_ -= delta_zoom;
            global_zoom_factor_ = new_global_zoom_factor;
          }
        }
        break;
      case SDL_MOUSEMOTION:
        SDL_GetMouseState(&mouse_x_, &mouse_y_);

        if (selection_state_ == SelectionState::STARTED) {
          selection_end_ = get_mouse_video_position(mouse_x_, mouse_y_, compute_zoom_rect());

          if (selection_wrap_) {
            selection_end_ = wrap_to_left_frame(selection_end_);
          }
        }

        if (event_.motion.state & SDL_BUTTON_RMASK) {
          const auto pan_offset = Vector2D(event_.motion.xrel, event_.motion.yrel) * Vector2D(video_to_window_width_factor_, video_to_window_height_factor_) / Vector2D(drawable_to_window_width_factor_, drawable_to_window_height_factor_);

          update_move_offset(move_offset_ + pan_offset);
        }

        if (show_metadata_) {
          handle_scroll(metadata_y_offset_, metadata_total_height_, metadata_textures_);
        }

        if (show_help_) {
          handle_scroll(help_y_offset_, help_total_height_, help_textures_);
        }
        break;
      case SDL_MOUSEBUTTONDOWN:
        if (event_.button.button == SDL_BUTTON_LEFT && save_selected_area_ && selection_state_ == SelectionState::NONE) {
          selection_state_ = SelectionState::STARTED;
          selection_start_ = get_mouse_video_position(mouse_x_, mouse_y_, compute_zoom_rect());

          // Check if the selection is outside the left video frame
          selection_wrap_ = (mode_ == Mode::HSTACK && selection_start_.x() >= video_width_) || (mode_ == Mode::VSTACK && selection_start_.y() >= video_height_);

          if (selection_wrap_) {
            selection_start_ = wrap_to_left_frame(selection_start_);
          }

          selection_end_ = selection_start_;
        } else if (event_.button.button != SDL_BUTTON_RIGHT) {
          seek_relative_ = static_cast<float>(mouse_x_) / static_cast<float>(window_width_);
          seek_from_start_ = true;
        }
        update_cursor();
        break;
      case SDL_MOUSEBUTTONUP:
        if (event_.button.button == SDL_BUTTON_LEFT && selection_state_ == SelectionState::STARTED) {
          selection_state_ = SelectionState::COMPLETED;
        }
        update_cursor();
        break;
      case SDL_KEYDOWN:
        switch (keycode) {
          case SDLK_h:
            show_help_ = !show_help_;
            break;
          case SDLK_ESCAPE:
            quit_ = true;
            break;
          case SDLK_SPACE:
            play_ = !play_;
            buffer_play_loop_mode_ = Loop::OFF;
            tick_playback_ = play_;
            break;
          case SDLK_COMMA:
          case SDLK_KP_COMMA:
            set_buffer_play_loop_mode(buffer_play_loop_mode_ != Loop::PINGPONG ? Loop::PINGPONG : Loop::OFF);
            break;
          case SDLK_PERIOD:
            set_buffer_play_loop_mode(buffer_play_loop_mode_ != Loop::FORWARDONLY ? Loop::FORWARDONLY : Loop::OFF);
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
          case SDLK_c: {
            if (is_clipboard_mod_pressed()) {
              const float previous_left_frame_secs = previous_left_frame_pts_ * AV_TIME_TO_SEC;
              const std::string previous_left_frame_secs_str = format_position(previous_left_frame_secs, false);

              SDL_SetClipboardText(previous_left_frame_secs_str.c_str());

              std::cout << "Copied to clipboard: " << previous_left_frame_secs_str << std::endl;
            } else {
              zoom_right_ = true;
            }
            break;
          }
          case SDLK_v: {
            if (is_clipboard_mod_pressed()) {
              char* clip_text = SDL_GetClipboardText();

              if (!clip_text) {
                std::cerr << "Failed to get clipboard text: " << SDL_GetError() << std::endl;
                return;
              }

              std::string clipboard_str(clip_text);
              SDL_free(clip_text);

              static const std::regex timestamp_regex(R"((?:(\d+):)?(?:(\d+):)?(\d+(?:\.\d+)?))");
              std::smatch match;

              if (std::regex_search(clipboard_str, match, timestamp_regex)) {
                std::string timestamp = match.str();
                std::cout << "Timestamp pasted: " << timestamp << std::endl;

                seek_relative_ = parse_timestamps_to_seconds(timestamp) / static_cast<float>(duration_);
                seek_from_start_ = true;
              } else {
                std::cout << "No valid timestamp found in clipboard." << std::endl;
              }
            } else {
              show_metadata_ = !show_metadata_;
            }
            break;
          }
          case SDLK_a:
            if (keymod & KMOD_SHIFT) {
              std::cerr << "Frame-accurate backward navigation has not yet been implemented" << std::endl;
            } else {
              frame_buffer_offset_delta_++;
            }
            break;
          case SDLK_d:
            if (keymod & KMOD_SHIFT) {
              frame_navigation_delta_++;
            } else {
              frame_buffer_offset_delta_--;
            }
            break;
          case SDLK_i:
            fast_input_alignment_ = !fast_input_alignment_;
            std::cout << "Input alignment resizing filter set to '" << (fast_input_alignment_ ? "BILINEAR (fast)" : "BICUBIC (high-quality)") << "' (takes effect for the next decoded frame)" << std::endl;
            break;
          case SDLK_t:
            bilinear_texture_filtering_ = !bilinear_texture_filtering_;
            std::cout << "Video texture filter set to '" << (bilinear_texture_filtering_ ? "BILINEAR" : "NEAREST NEIGHBOR") << "'" << std::endl;
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
            if (keymod & KMOD_SHIFT) {
              if (!save_selected_area_) {
                save_selected_area_ = true;
              } else {
                save_selected_area_ = false;
                selection_state_ = SelectionState::NONE;
              }
              update_cursor();
            } else {
              save_image_frames_ = true;
            }
            break;
          case SDLK_p:
            print_mouse_position_and_color_ = mouse_is_inside_window_;
            break;
          case SDLK_m:
            print_image_similarity_metrics_ = true;
            break;
          case SDLK_4:
          case SDLK_KP_4:
            update_zoom_factor_and_move_offset(std::min(video_to_window_width_factor_ / drawable_to_window_width_factor_, video_to_window_height_factor_ / drawable_to_window_height_factor_));
            break;
          case SDLK_5:
          case SDLK_KP_5:
            update_zoom_factor_and_move_offset(0.5F);
            break;
          case SDLK_6:
          case SDLK_KP_6:
            update_zoom_factor_and_move_offset(1.0F);
            break;
          case SDLK_7:
          case SDLK_KP_7:
            update_zoom_factor_and_move_offset(2.0F);
            break;
          case SDLK_8:
          case SDLK_KP_8:
            update_zoom_factor_and_move_offset(4.0F);
            break;
          case SDLK_9:
          case SDLK_KP_9:
            update_zoom_factor_and_move_offset(8.0F);
            break;
          case SDLK_r:
            update_zoom_factor(1.0F);
            move_offset_ = Vector2D(0.0F, 0.0F);
            global_center_ = Vector2D(0.5F, 0.5F);
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
          case SDLK_j:
            update_playback_speed(playback_speed_level_ - 1);
            possibly_tick_playback_ = true;
            break;
          case SDLK_l:
            update_playback_speed(playback_speed_level_ + 1);
            tick_playback_ = true;
            break;
          case SDLK_x:
            show_fps_ = true;
            break;
          case SDLK_PLUS:
          case SDLK_KP_PLUS:
          case SDLK_EQUALS:  // for tenkeyless keyboards
            if (keymod & KMOD_ALT) {
              shift_right_frames_ += 100;
            } else if (keymod & KMOD_CTRL) {
              shift_right_frames_ += 10;
            } else {
              shift_right_frames_++;
            }
            break;
          case SDLK_MINUS:
          case SDLK_KP_MINUS:
            if (keymod & KMOD_ALT) {
              shift_right_frames_ -= 100;
            } else if (keymod & KMOD_CTRL) {
              shift_right_frames_ -= 10;
            } else {
              shift_right_frames_--;
            }
            break;
          case SDLK_y:
            // Cycle through subtraction modes
            switch (diff_mode_) {
              case DiffMode::LegacyAbs:
                diff_mode_ = DiffMode::AbsLinear;
                break;
              case DiffMode::AbsLinear:
                diff_mode_ = DiffMode::AbsSqrt;
                break;
              case DiffMode::AbsSqrt:
                diff_mode_ = DiffMode::SignedDiverging;
                break;
              case DiffMode::SignedDiverging:
                diff_mode_ = DiffMode::LegacyAbs;
                break;
            }
            std::cout << "Subtraction mode set to '";
            switch (diff_mode_) {
              case DiffMode::LegacyAbs:
                std::cout << "ABSOLUTE LINEAR (FIXED GAIN)";
                break;
              case DiffMode::AbsLinear:
                std::cout << "ABSOLUTE LINEAR (ADAPTIVE)";
                break;
              case DiffMode::AbsSqrt:
                std::cout << "ABSOLUTE SQUARE ROOT";
                break;
              case DiffMode::SignedDiverging:
                std::cout << "SIGNED DIVERGING";
                break;
            }
            std::cout << "'" << std::endl;
            break;
          case SDLK_u:
            diff_luma_only_ = !diff_luma_only_;
            std::cout << "Subtraction luminance-only set to '" << (diff_luma_only_ ? "ON" : "OFF") << "'" << std::endl;
            break;
          default:
            break;
        }
        break;
      case SDL_KEYUP:
        switch (keycode) {
          case SDLK_z:
            zoom_left_ = false;
            break;
          case SDLK_c:
            zoom_right_ = false;
            break;
          case SDLK_x:
            show_fps_ = false;
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

Display::Loop Display::get_buffer_play_loop_mode() const {
  return buffer_play_loop_mode_;
}

void Display::set_buffer_play_loop_mode(const Display::Loop& mode) {
  buffer_play_loop_mode_ = mode;
  play_ = false;
  tick_playback_ = true;

  if (mode == Loop::FORWARDONLY) {
    buffer_play_forward_ = true;
  }
}

bool Display::get_buffer_play_forward() const {
  return buffer_play_forward_;
}

void Display::toggle_buffer_play_direction() {
  buffer_play_forward_ = !buffer_play_forward_;
}

bool Display::get_fast_input_alignment() const {
  return fast_input_alignment_;
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

int Display::get_frame_buffer_offset_delta() const {
  return frame_buffer_offset_delta_;
}

int Display::get_frame_navigation_delta() const {
  return frame_navigation_delta_;
}

int Display::get_shift_right_frames() const {
  return shift_right_frames_;
}

float Display::get_playback_speed_factor() const {
  return playback_speed_factor_;
}

bool Display::get_tick_playback() const {
  return tick_playback_;
}

bool Display::get_possibly_tick_playback() const {
  return possibly_tick_playback_;
}

bool Display::get_show_fps() const {
  return show_fps_;
}
