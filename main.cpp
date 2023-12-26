#define SDL_MAIN_HANDLED
#include <algorithm>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <vector>
#include "argagg.h"
#include "string_utils.h"
#include "video_compare.h"

#ifdef _WIN32
#include <Windows.h>

// Credits to Mircea Neacsu, https://github.com/neacsum/utf8
char** get_argv(int* argc, char** argv) {
  char** uargv = nullptr;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), argc);
  if (wargv) {
    uargv = new char*[*argc];
    for (int i = 0; i < *argc; i++) {
      int nc = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, 0, 0, 0, 0);
      uargv[i] = new char[nc + 1];
      WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, uargv[i], nc, 0, 0);
    }
    LocalFree(wargv);
  }
  return uargv;
}

void free_argv(int argc, char** argv) {
  for (int i = 0; i < argc; i++) {
    delete argv[i];
  }
  delete argv;
}
#else
#define UNUSED(x) (void)(x)

char** get_argv(const int* argc, char** argv) {
  UNUSED(argc);
  return argv;
}

void free_argv(int argc, char** argv) {
  UNUSED(argc);
  UNUSED(argv);
}
#endif

static const std::string REPEAT_FILE_NAME("__");

void print_controls() {
  const std::vector<std::pair<std::string, std::string>> controls{{"Space", "Toggle play/pause"},
                                                                  {",", "Toggle bidirectional in-buffer loop/pause"},
                                                                  {".", "Toggle forward-only in-buffer loop/pause"},
                                                                  {"Escape", "Quit"},
                                                                  {"Down arrow", "Seek 15 seconds backward"},
                                                                  {"Left arrow", "Seek 1 second backward"},
                                                                  {"Page down", "Seek 600 seconds backward"},
                                                                  {"Up arrow", "Seek 15 seconds forward"},
                                                                  {"Right arrow", "Seek 1 second forward"},
                                                                  {"Page up", "Seek 600 seconds forward"},
                                                                  {"S", "Swap left and right video"},
                                                                  {"A", "Previous frame"},
                                                                  {"D", "Next frame"},
                                                                  {"F", "Save both frames as PNG images in the current directory"},
                                                                  {"P", "Print mouse position and pixel value under cursor to console"},
                                                                  {"Z", "Magnify area around cursor (result shown in lower left corner)"},
                                                                  {"C", "Magnify area around cursor (result shown in lower right corner)"},
                                                                  {"R", "Re-center and reset zoom to 100% (x1)"},
                                                                  {"1", "Toggle hide/show left video"},
                                                                  {"2", "Toggle hide/show right video"},
                                                                  {"3", "Toggle hide/show HUD"},
                                                                  {"5", "Zoom  50% (x0.5)"},
                                                                  {"6", "Zoom 100% (x1)"},
                                                                  {"7", "Zoom 200% (x2)"},
                                                                  {"8", "Zoom 400% (x4)"},
                                                                  {"0", "Toggle video/subtraction mode"},
                                                                  {"+", "Time-shift right video 1 frame forward"},
                                                                  {"-", "Time-shift right video 1 frame backward"}};

  std::cout << "Controls:" << std::endl << std::endl;

  for (auto& key_description_pair : controls) {
    std::cout << string_sprintf(" %-12s %s", key_description_pair.first.c_str(), key_description_pair.second.c_str()) << std::endl;
  }

  std::cout << std::endl << "Move the mouse horizontally to adjust the movable slider position." << std::endl << std::endl;

  std::cout << "Use the mouse wheel to zoom in/out on the pixel under the cursor. Pan the view" << std::endl;
  std::cout << "by moving the mouse while holding down the right button." << std::endl << std::endl;

  std::cout << "Left-click the mouse to perform a time seek based on the horizontal position" << std::endl;
  std::cout << "of the mouse cursor relative to the window width (the target position is" << std::endl;
  std::cout << "shown in the lower right corner)." << std::endl << std::endl;

  std::cout << "Hold CTRL while time-shifting with +/- for faster increments/decrements" << std::endl;
  std::cout << "of 10 frames per keystroke. Similarly, hold down the ALT key for even" << std::endl;
  std::cout << "bigger time-shifts of 100 frames." << std::endl;
}

void find_matching_video_demuxers(const std::string& search_string) {
  const AVInputFormat* demuxer = nullptr;
  void* i = 0;

  std::cout << "Demuxers:" << std::endl << std::endl;

  while ((demuxer = av_demuxer_iterate(&i))) {
    std::string demuxer_name(demuxer->name);
    std::string demuxer_long_name(demuxer->long_name);

    auto name_it = string_ci_find(demuxer_name, search_string);
    auto long_name_it = string_ci_find(demuxer_long_name, search_string);

    if (name_it != demuxer_name.end() || long_name_it != demuxer_long_name.end()) {
      std::cout << string_sprintf(" %-24s %s", demuxer_name.c_str(), demuxer_long_name.c_str()) << std::endl;
    }
  }
}

void find_matching_video_decoders(const std::string& search_string) {
  const AVCodec* codec = nullptr;
  void* i = 0;

  std::cout << "Decoders:" << std::endl;
  std::cout << " A. = Backed by hardware implementation" << std::endl;
  std::cout << " .Y = Potentially backed by a hardware implementation, but not necessarily" << std::endl << std::endl;

  while ((codec = av_codec_iterate(&i))) {
    if (codec->type == AVMEDIA_TYPE_VIDEO && av_codec_is_decoder(codec)) {
      std::string codec_name(codec->name);
      std::string codec_long_name(codec->long_name);

      auto name_it = string_ci_find(codec_name, search_string);
      auto long_name_it = string_ci_find(codec_long_name, search_string);

      if (name_it != codec_name.end() || long_name_it != codec_long_name.end()) {
        std::string capability = (codec->capabilities & AV_CODEC_CAP_HARDWARE) ? "A" : ".";
        capability += (codec->capabilities & AV_CODEC_CAP_HYBRID) ? "Y" : ".";

        std::cout << string_sprintf(" %s %-18s %s", capability.c_str(), codec_name.c_str(), codec_long_name.c_str()) << std::endl;
      }
    }
  }
}

void find_matching_hw_accels(const std::string& search_string) {
  AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;

  std::cout << "Hardware acceleration methods:" << std::endl << std::endl;

  while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
    std::string hw_accel_method(av_hwdevice_get_type_name(type));

    auto name_it = string_ci_find(hw_accel_method, search_string);

    if (name_it != hw_accel_method.end()) {
      std::cout << string_sprintf(" %s", hw_accel_method.c_str()) << std::endl;
    }
  }
}

int main(int argc, char** argv) {
  char** argv_decoded = get_argv(&argc, argv);
  int exit_code = 0;

#if (LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 6, 102))
  av_register_all();
  avcodec_register_all();
#endif

  try {
    argagg::parser argparser{{{"help", {"-h", "--help"}, "show help", 0},
                              {"show-controls", {"-c", "--show-controls"}, "show controls", 0},
                              {"verbose", {"-v", "--verbose"}, "enable verbose output, including information such as library versions and rendering details", 0},
                              {"high-dpi", {"-d", "--high-dpi"}, "allow high DPI mode for e.g. displaying UHD content on Retina displays", 0},
                              {"10-bpc", {"-b", "--10-bpc"}, "use 10 bits per color component instead of 8", 0},
                              {"display-number", {"-n", "--display-number"}, "open main window on specific display (e.g. 0, 1 or 2), default is 0", 1},
                              {"display-mode", {"-m", "--mode"}, "display mode (layout), 'split' for split screen (default), 'vstack' for vertical stack, 'hstack' for horizontal stack", 1},
                              {"window-size", {"-w", "--window-size"}, "override window size, specified as [width]x[height] (e.g. 800x600, 1280x or x480)", 1},
                              {"frame-buffer-size", {"-f", "--frame-buffer-size"}, "frame buffer size (e.g. 10, 70 or 150), default is 50", 1},
                              {"time-shift", {"-t", "--time-shift"}, "shift the time stamps of the right video by a user-specified number of seconds (e.g. 0.150, -0.1 or 1)", 1},
                              {"wheel-sensitivity", {"-s", "--wheel-sensivitiy"}, "mouse wheel sensitivity (e.g. 0.5, -1 or 1.7), default is 1; negative values invert the input direction", 1},
                              {"left-filters", {"-l", "--left-filters"}, "specify a comma-separated list of FFmpeg filters to be applied to the left video (e.g. format=gray,crop=iw:ih-240)", 1},
                              {"right-filters", {"-r", "--right-filters"}, "specify a comma-separated list of FFmpeg filters to be applied to the right video (e.g. yadif,hqdn3d,pad=iw+320:ih:160:0)", 1},
                              {"left-demuxer", {"--left-demuxer"}, "left FFmpeg video demuxer name", 1},
                              {"right-demuxer", {"--right-demuxer"}, "right FFmpeg video demuxer name", 1},
                              {"find-demuxers", {"--find-demuxers"}, "find FFmpeg video demuxers matching the provided search term (e.g. 'matroska', 'mp4', 'vapoursynth' or 'pipe'; use \"\" to list all)", 1},
                              {"left-decoder", {"--left-decoder"}, "left FFmpeg video decoder name", 1},
                              {"right-decoder", {"--right-decoder"}, "right FFmpeg video decoder name", 1},
                              {"find-decoders", {"--find-decoders"}, "find FFmpeg video decoders matching the provided search term (e.g. 'h264', 'hevc', 'av1' or 'cuvid'; use \"\" to list all)", 1},
                              {"left-hwaccel", {"--left-hwaccel"}, "left FFmpeg video hardware acceleration, specified as [type][:device?] (e.g. 'videotoolbox' or 'vaapi:/dev/dri/renderD128')", 1},
                              {"right-hwaccel", {"--right-hwaccel"}, "right FFmpeg video hardware acceleration, specified as [type][:device?] (e.g. 'cuda', 'cuda:1' or 'vulkan')", 1},
                              {"find-hwaccels", {"--find-hwaccels"}, "find FFmpeg video hardware acceleration types matching the provided search term (e.g. 'videotoolbox' or 'vulkan'; use \"\" to list all)", 1},
                              {"disable-auto-filters", {"--no-auto-filters"}, "disable the default behaviour of automatically injecting filters for deinterlacing, frame rate harmonization, and rotation", 0}}};

    argagg::parser_results args;
    args = argparser.parse(argc, argv_decoded);

    int display_number = 0;
    size_t frame_buffer_size = 50;
    Display::Mode display_mode = Display::Mode::split;
    std::tuple<int, int> window_size(-1, -1);
    double time_shift_ms = 0;
    float wheel_sensitivity = 1;
    std::string left_video_filters, right_video_filters;
    std::string left_demuxer, right_demuxer;
    std::string left_decoder, right_decoder;
    std::string left_hw_accel_spec, right_hw_accel_spec;

    if (args["show-controls"]) {
      print_controls();
    } else if (args["find-demuxers"]) {
      find_matching_video_demuxers(args["find-demuxers"]);
    } else if (args["find-decoders"]) {
      find_matching_video_decoders(args["find-decoders"]);
    } else if (args["find-hwaccels"]) {
      find_matching_hw_accels(args["find-hwaccels"]);
    } else if (args["help"] || args.count() == 0) {
      std::ostringstream usage;
      usage << "video-compare 20231224-mumbai Copyright (c) 2018-2023 Jon Frydensbjerg, the video-compare community" << std::endl << std::endl;
      usage << "Usage: " << argv[0] << " [OPTIONS]... FILE1 FILE2" << std::endl << std::endl;
      argagg::fmt_ostream fmt(std::cerr);
      fmt << usage.str() << argparser;
    } else {
      if (args.pos.size() != 2) {
        throw std::logic_error{"Two FFmpeg compatible video files must be supplied"};
      }
      if (args["display-number"]) {
        const std::string display_number_arg = args["display-number"];
        const std::regex display_number_re("(\\d*)");

        if (!std::regex_match(display_number_arg, display_number_re)) {
          throw std::logic_error{"Cannot parse display number argument (required format: [number], e.g. 0, 1 or 2)"};
        }

        display_number = std::stoi(display_number_arg);
      }
      if (args["display-mode"]) {
        const std::string display_mode_arg = args["display-mode"];

        if (display_mode_arg == "split") {
          display_mode = Display::Mode::split;
        } else if (display_mode_arg == "vstack") {
          display_mode = Display::Mode::vstack;
        } else if (display_mode_arg == "hstack") {
          display_mode = Display::Mode::hstack;
        } else {
          throw std::logic_error{"Cannot parse display mode argument (valid options: split, vstack, hstack)"};
        }
      }
      if (args["window-size"]) {
        const std::string window_size_arg = args["window-size"];
        const std::regex window_size_re("(\\d*)x(\\d*)");

        if (!std::regex_match(window_size_arg, window_size_re)) {
          throw std::logic_error{"Cannot parse window size argument (required format: [width]x[height], e.g. 800x600, 1280x or x480)"};
        }

        const std::regex delimiter_re("x");

        auto const token_vec = std::vector<std::string>(std::sregex_token_iterator{begin(window_size_arg), end(window_size_arg), delimiter_re, -1}, std::sregex_token_iterator{});

        window_size = std::make_tuple(!token_vec[0].empty() ? std::stoi(token_vec[0]) : -1, token_vec.size() == 2 ? std::stoi(token_vec[1]) : -1);
      }
      if (args["frame-buffer-size"]) {
        const std::string frame_buffer_size_arg = args["frame-buffer-size"];
        const std::regex frame_buffer_size_re("(\\d*)");

        if (!std::regex_match(frame_buffer_size_arg, frame_buffer_size_re)) {
          throw std::logic_error{"Cannot parse frame buffer size (required format: [number], e.g. 10, 70 or 150)"};
        }

        frame_buffer_size = std::stoi(frame_buffer_size_arg);

        if (frame_buffer_size < 1) {
          throw std::logic_error{"Frame buffer size must be at least 1"};
        }
      }
      if (args["time-shift"]) {
        const std::string time_shift_arg = args["time-shift"];
        const std::regex time_shift_re("^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)$");

        if (!std::regex_match(time_shift_arg, time_shift_re)) {
          throw std::logic_error{"Cannot parse time shift argument; must be a valid number in seconds, e.g. 1 or -0.333"};
        }

        time_shift_ms = std::stod(time_shift_arg) * 1000.0;
      }
      if (args["wheel-sensitivity"]) {
        const std::string wheel_sensitivity_arg = args["wheel-sensitivity"];
        const std::regex wheel_sensitivity_re("^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)$");

        if (!std::regex_match(wheel_sensitivity_arg, wheel_sensitivity_re)) {
          throw std::logic_error{"Cannot parse wheel sensitivity argument; must be a valid number, e.g. 1.3 or -1"};
        }

        wheel_sensitivity = std::stod(wheel_sensitivity_arg);
      }
      if (args["left-filters"]) {
        left_video_filters = static_cast<const std::string&>(args["left-filters"]);
      }
      if (args["right-filters"]) {
        right_video_filters = static_cast<const std::string&>(args["right-filters"]);
      }
      if (args["left-demuxer"]) {
        left_demuxer = static_cast<const std::string&>(args["left-demuxer"]);
      }
      if (args["right-demuxer"]) {
        right_demuxer = static_cast<const std::string&>(args["right-demuxer"]);
      }
      if (args["left-decoder"]) {
        left_decoder = static_cast<const std::string&>(args["left-decoder"]);
      }
      if (args["right-decoder"]) {
        right_decoder = static_cast<const std::string&>(args["right-decoder"]);
      }
      if (args["left-hwaccel"]) {
        left_hw_accel_spec = static_cast<const std::string&>(args["left-hwaccel"]);
      }
      if (args["right-hwaccel"]) {
        right_hw_accel_spec = static_cast<const std::string&>(args["right-hwaccel"]);
      }

      std::string left_file_name = args.pos[0];
      std::string right_file_name = args.pos[1];

      if (left_file_name == REPEAT_FILE_NAME && right_file_name == REPEAT_FILE_NAME) {
        throw std::logic_error{"At least one actual video file must be supplied"};
      } else if (left_file_name == REPEAT_FILE_NAME) {
        left_file_name = right_file_name;
      } else if (right_file_name == REPEAT_FILE_NAME) {
        right_file_name = left_file_name;
      }

      VideoCompare compare{
          display_number, display_mode,       args["verbose"], args["high-dpi"],    args["10-bpc"], window_size,   frame_buffer_size,   time_shift_ms, wheel_sensitivity, left_file_name, left_video_filters, left_demuxer,
          left_decoder,   left_hw_accel_spec, right_file_name, right_video_filters, right_demuxer,  right_decoder, right_hw_accel_spec, args["disable-auto-filters"]};
      compare();
    }
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    exit_code = -1;
  }

  free_argv(argc, argv_decoded);

  return exit_code;
}
