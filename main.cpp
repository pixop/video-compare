#define SDL_MAIN_HANDLED
#include <algorithm>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>
#include "argagg.h"
#include "video_compare.h"
#include "string_utils.h"

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

void print_controls() {
  const std::vector<std::pair<std::string, std::string>> controls{{"Space", "Toggle play/pause"},
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
                                                                  {"Z", "Zoom area around cursor (result shown in lower left corner)"},
                                                                  {"C", "Zoom area around cursor (result shown in lower right corner)"},
                                                                  {"1", "Toggle hide/show left video"},
                                                                  {"2", "Toggle hide/show right video"},
                                                                  {"3", "Toggle hide/show HUD"},
                                                                  {"0", "Toggle video/subtraction mode"},
                                                                  {"+", "Time-shift right video 1 frame forward"},
                                                                  {"-", "Time-shift right video 1 frame backward"}};

  std::cout << "Controls: " << std::endl << std::endl;

  for (auto& key_description_pair : controls) {
    std::cout << "- " << key_description_pair.first << ": " << key_description_pair.second << std::endl;
  }

  std::cout << std::endl << "Move the mouse horizontally to adjust the movable slider position." << std::endl << std::endl;

  std::cout << "Click the mouse to perform a time seek based on the horizontal position" << std::endl;
  std::cout << "of the mouse cursor relative to the window width (the target position is" << std::endl;
  std::cout << "shown in the lower right corner)." << std::endl << std::endl;

  std::cout << "Hold CTRL while time-shifting with +/- for faster increments/decrements" << std::endl;
  std::cout << "of 10 frames per keystroke. Similarly, hold down the ALT key for even" << std::endl;
  std::cout << "bigger time-shifts of 100 frames." << std::endl;
}

void find_matching_video_decoders(const std::string &search_string) {
  const AVCodec *codec = nullptr;
  void *i = 0;

  std::cout << "Decoders:" << std::endl;
  std::cout << " A. = Backed by hardware implementation" << std::endl;
  std::cout << " .Y = Potentially backed by a hardware implementation, but not necessarily" << std::endl << std::endl;

  while ((codec = av_codec_iterate(&i)))
  {
    if (codec->type == AVMEDIA_TYPE_VIDEO && av_codec_is_decoder(codec)) {
      auto name_pos = strcasestr(codec->name, search_string.c_str());
      auto long_name_pos = strcasestr(codec->long_name, search_string.c_str());

      if (name_pos != nullptr || long_name_pos != nullptr) {
        std::string capability = (codec->capabilities & AV_CODEC_CAP_HARDWARE) ? "A" : ".";
        capability += (codec->capabilities & AV_CODEC_CAP_HYBRID) ? "Y" : ".";

        std::cout << string_sprintf(" %s %-18s %s", capability.c_str(), codec->name, codec->long_name) << std::endl;
      }
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
                              {"high-dpi", {"-d", "--high-dpi"}, "allow high DPI mode for e.g. displaying UHD content on Retina displays", 0},
                              {"10-bpc", {"-b", "--10-bpc"}, "use 10 bits per color component instead of 8", 0},
                              {"display-number", {"-n", "--display-number"}, "open main window on specific display (e.g. 0, 1 or 2), default is 0", 1},
                              {"display-mode", {"-m", "--mode"}, "display mode (layout), 'split' for split screen (default), 'vstack' for vertical stack, 'hstack' for horizontal stack", 1},
                              {"window-size", {"-w", "--window-size"}, "override window size, specified as [width]x[height] (e.g. 800x600, 1280x or x480)", 1},
                              {"time-shift", {"-t", "--time-shift"}, "shift the time stamps of the right video by a user-specified number of seconds (e.g. 0.150, -0.1 or 1)", 1},
                              {"left-filters", {"-l", "--left-filters"}, "specify a comma-separated list of FFmpeg filters to be applied to the left video (e.g. format=gray,crop=iw:ih-240)", 1},
                              {"right-filters", {"-r", "--right-filters"}, "specify a comma-separated list of FFmpeg filters to be applied to the right video (e.g. yadif,hqdn3d,pad=iw+320:ih:160:0)", 1},
                              {"left-decoder", {"--left-decoder"}, "left FFmpeg video decoder name (e.g. h264 or h264_cuvid)", 1},
                              {"right-decoder", {"--right-decoder"}, "right FFmpeg video decoder name (e.g. h264 or h264_cuvid)", 1},
                              {"find-decoders", {"--find-decoders"}, "find FFmpeg video decoders matching the provided search term (e.g. 'h264', 'hevc' or 'av1'; use \"\" to list all)", 1}}};

    argagg::parser_results args;
    args = argparser.parse(argc, argv_decoded);

    int display_number = 0;
    Display::Mode display_mode = Display::Mode::split;
    std::tuple<int, int> window_size(-1, -1);
    double time_shift_ms = 0;
    std::string left_video_filters, right_video_filters;
    std::string left_decoder, right_decoder;

    if (args["show-controls"]) {
      print_controls();
    } else if (args["find-decoders"]) {
      find_matching_video_decoders(args["find-decoders"]);
    } else if (args["help"] || args.count() == 0) {
      std::ostringstream usage;
      usage << "video-compare 20230709-github Copyright (c) 2018-2023 Jon Frydensbjerg, the video-compare community" << std::endl << std::endl;
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
      if (args["time-shift"]) {
        const std::string time_shift_arg = args["time-shift"];
        const std::regex time_shift_re("^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)$");

        if (!std::regex_match(time_shift_arg, time_shift_re)) {
          throw std::logic_error{"Cannot parse time shift argument; must be a valid number in seconds, e.g. 1 or -0.333"};
        }

        time_shift_ms = std::stod(time_shift_arg) * 1000.0;
      }
      if (args["left-filters"]) {
        left_video_filters = static_cast<const std::string&>(args["left-filters"]);
      }
      if (args["right-filters"]) {
        right_video_filters = static_cast<const std::string&>(args["right-filters"]);
      }
      if (args["left-decoder"]) {
        left_decoder = static_cast<const std::string&>(args["left-decoder"]);
      }
      if (args["right-decoder"]) {
        right_decoder = static_cast<const std::string&>(args["right-decoder"]);
      }

      VideoCompare compare{display_number, display_mode, args["high-dpi"], args["10-bpc"], window_size, time_shift_ms, args.pos[0], left_video_filters, left_decoder, args.pos[1], right_video_filters, right_decoder};
      compare();
    }
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    exit_code = -1;
  }

  free_argv(argc, argv_decoded);

  return exit_code;
}
