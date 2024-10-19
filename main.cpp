#define SDL_MAIN_HANDLED
#include <algorithm>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <vector>
#include "argagg.h"
#include "controls.h"
#include "string_utils.h"
#include "version.h"
#include "video_compare.h"
#include "vmaf_calculator.h"

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
  std::cout << "Controls:" << std::endl << std::endl;

  for (auto& key_description_pair : get_controls()) {
    std::cout << string_sprintf(" %-12s %s", key_description_pair.first.c_str(), key_description_pair.second.c_str()) << std::endl;
  }

  for (auto& instruction : get_instructions()) {
    std::cout << std::endl;

    print_wrapped(instruction, 80);
  }
}

void find_matching_video_filters(const std::string& search_string) {
  const AVFilter* filter = nullptr;
  void* i = 0;

  std::cout << "Filters:" << std::endl << std::endl;

  while ((filter = av_filter_iterate(&i))) {
#if (LIBAVFILTER_VERSION_INT < AV_VERSION_INT(8, 24, 100))
    if (avfilter_pad_count(filter->inputs) >= 1 && avfilter_pad_count(filter->outputs) >= 1) {
#else
    if (avfilter_filter_pad_count(filter, 0) >= 1 && avfilter_filter_pad_count(filter, 1) >= 1) {
#endif
      if (avfilter_pad_get_type(filter->inputs, 0) == AVMEDIA_TYPE_VIDEO && avfilter_pad_get_type(filter->outputs, 0) == AVMEDIA_TYPE_VIDEO) {
        std::string filter_name(filter->name);
        std::string filter_description(filter->description);

        auto name_it = string_ci_find(filter_name, search_string);
        auto description_it = string_ci_find(filter_description, search_string);

        if (name_it != filter_name.end() || description_it != filter_description.end()) {
          std::cout << string_sprintf(" %-20s %s", filter_name.c_str(), filter_description.c_str()) << std::endl;
        }
      }
    }
  }
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

void find_matching_input_protocols(const std::string& search_string) {
  const char* protocol = nullptr;
  void* i = 0;

  std::cout << "Input protocols:" << std::endl << std::endl;

  while ((protocol = avio_enum_protocols(&i, 0))) {
    std::string protocol_name(protocol);

    auto name_it = string_ci_find(protocol_name, search_string);

    if (name_it != protocol_name.end()) {
      std::cout << string_sprintf(" %s", protocol_name.c_str()) << std::endl;
    }
  }
}

void find_matching_video_decoders(const std::string& search_string) {
  const AVCodec* codec = nullptr;
  void* i = 0;

  std::cout << "Decoders:" << std::endl;
  std::cout << " A.. = Backed by hardware implementation" << std::endl;
  std::cout << " .Y. = Potentially backed by a hardware implementation, but not necessarily" << std::endl;
  std::cout << " ..X = Decoder is experimental" << std::endl << std::endl;

  while ((codec = av_codec_iterate(&i))) {
    if (codec->type == AVMEDIA_TYPE_VIDEO && av_codec_is_decoder(codec)) {
      std::string codec_name(codec->name);
      std::string codec_long_name(codec->long_name);

      auto name_it = string_ci_find(codec_name, search_string);
      auto long_name_it = string_ci_find(codec_long_name, search_string);

      if (name_it != codec_name.end() || long_name_it != codec_long_name.end()) {
        std::string capability = (codec->capabilities & AV_CODEC_CAP_HARDWARE) ? "A" : ".";
        capability += (codec->capabilities & AV_CODEC_CAP_HYBRID) ? "Y" : ".";
        capability += (codec->capabilities & AV_CODEC_CAP_EXPERIMENTAL) ? "X" : ".";

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

const std::string get_nth_token_or_empty(const std::string& options_string, const char delimiter, const size_t n) {
  auto tokens = string_split(options_string, delimiter);

  return tokens.size() > n ? tokens[n] : "";
}

AVDictionary* upsert_avdict_options(AVDictionary* dict, const std::string& options_string) {
  auto options = string_split(options_string, ',');

  for (auto option : options) {
    auto key_value_pair = string_split(option, '=');

    if (key_value_pair.size() != 2) {
      throw std::logic_error{"key=value expected for option"};
    }

    av_dict_set(&dict, key_value_pair[0].c_str(), key_value_pair[1].c_str(), 0);
  }

  return dict;
}

AVDictionary* create_default_demuxer_options() {
  AVDictionary* demuxer_options = nullptr;
  av_dict_set(&demuxer_options, "analyzeduration", "100000000", 0);
  av_dict_set(&demuxer_options, "probesize", "100000000", 0);

  return demuxer_options;
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
                              {"window-fit-display", {"-W", "--window-fit-display"}, "calculate the window size to fit within the usable display bounds while maintaining the video aspect ratio", 0},
                              {"auto-loop-mode", {"-a", "--auto-loop-mode"}, "auto-loop playback when buffer fills, 'off' for continuous streaming (default), 'on' for forward-only mode, 'pp' for ping-pong mode", 1},
                              {"frame-buffer-size", {"-f", "--frame-buffer-size"}, "frame buffer size (e.g. 10, 70 or 150), default is 50", 1},
                              {"time-shift", {"-t", "--time-shift"}, "shift the time stamps of the right video by a user-specified number of seconds (e.g. 0.150, -0.1 or 1)", 1},
                              {"wheel-sensitivity", {"-s", "--wheel-sensitivity"}, "mouse wheel sensitivity (e.g. 0.5, -1 or 1.7), default is 1; negative values invert the input direction", 1},
                              {"tone-map-mode", {"-T", "--tone-map-mode"}, "adapt tones to an sRGB display, 'off' for no conversion (default), 'on' for full-range tone mapping, 'rel' for relative tone comparison", 1},
                              {"left-peak-nits", {"-L", "--left-peak-nits"}, "left video peak luminance in nits (e.g. 850 or 1000), default is 100 for SDR; tone mapping is enabled if --tone-map-mode is not set", 1},
                              {"right-peak-nits", {"-R", "--right-peak-nits"}, "right video peak luminance in nits; see --left-peak-nits", 1},
                              {"boost-tone", {"-B", "--boost-tone"}, "adjust tone-mapping strength using a multiplication factor (e.g. 0.6 or 3), default is 1; tone mapping is enabled if --tone-map-mode is not set", 1},
                              {"left-filters", {"-l", "--left-filters"}, "specify a comma-separated list of FFmpeg filters to be applied to the left video (e.g. format=gray,crop=iw:ih-240)", 1},
                              {"right-filters", {"-r", "--right-filters"}, "specify a comma-separated list of FFmpeg filters to be applied to the right video (e.g. yadif,hqdn3d,pad=iw+320:ih:160:0)", 1},
                              {"find-filters", {"--find-filters"}, "find FFmpeg video filters that match the provided search term (e.g. 'scale', 'libvmaf' or 'dnn'; use \"\" to list all)", 1},
                              {"find-protocols", {"--find-protocols"}, "find FFmpeg input protocols that match the provided search term (e.g. 'ipfs', 'srt', or 'rtmp'; use \"\" to list all)", 1},
                              {"left-demuxer", {"--left-demuxer"}, "left FFmpeg video demuxer name, specified as [type?][:options?] (e.g. 'rawvideo:pixel_format=rgb24,video_size=320x240,framerate=10')", 1},
                              {"right-demuxer", {"--right-demuxer"}, "right FFmpeg video demuxer name, specified as [type?][:options?]", 1},
                              {"find-demuxers", {"--find-demuxers"}, "find FFmpeg video demuxers that match the provided search term (e.g. 'matroska', 'mp4', 'vapoursynth' or 'pipe'; use \"\" to list all)", 1},
                              {"left-decoder", {"--left-decoder"}, "left FFmpeg video decoder name, specified as [type?][:options?] (e.g. ':strict=unofficial', ':strict=-2' or 'vvc:strict=experimental')", 1},
                              {"right-decoder", {"--right-decoder"}, "right FFmpeg video decoder name, specified as [type?][:options?] (e.g. ':strict=-2,trust_dec_pts=1' or 'h264:trust_dec_pts=1')", 1},
                              {"find-decoders", {"--find-decoders"}, "find FFmpeg video decoders that match the provided search term (e.g. 'h264', 'hevc', 'av1' or 'cuvid'; use \"\" to list all)", 1},
                              {"left-hwaccel", {"--left-hwaccel"}, "left FFmpeg video hardware acceleration, specified as [type][:device?[:options?]] (e.g. 'videotoolbox' or 'vaapi:/dev/dri/renderD128')", 1},
                              {"right-hwaccel", {"--right-hwaccel"}, "right FFmpeg video hardware acceleration, specified as [type][:device?[:options?]] (e.g. 'cuda', 'cuda:1' or 'vulkan')", 1},
                              {"find-hwaccels", {"--find-hwaccels"}, "find FFmpeg video hardware acceleration types that match the provided search term (e.g. 'videotoolbox' or 'vulkan'; use \"\" to list all)", 1},
                              {"libvmaf-options", {"--libvmaf-options"}, "libvmaf FFmpeg filter options (e.g. 'model=version=vmaf_4k_v0.6.1' or 'model=version=vmaf_v0.6.1\\\\:name=hd|version=vmaf_4k_v0.6.1\\\\:name=4k')", 1},
                              {"disable-auto-filters", {"--no-auto-filters"}, "disable the default behaviour of automatically injecting filters for deinterlacing, DAR correction, frame rate harmonization, and rotation", 0}}};

    argagg::parser_results args;
    args = argparser.parse(argc, argv_decoded);

    if (args["show-controls"]) {
      print_controls();
    } else if (args["find-filters"]) {
      find_matching_video_filters(args["find-filters"]);
    } else if (args["find-demuxers"]) {
      find_matching_video_demuxers(args["find-demuxers"]);
    } else if (args["find-protocols"]) {
      find_matching_input_protocols(args["find-protocols"]);
    } else if (args["find-decoders"]) {
      find_matching_video_decoders(args["find-decoders"]);
    } else if (args["find-hwaccels"]) {
      find_matching_hw_accels(args["find-hwaccels"]);
    } else if (args["help"] || args.count() == 0) {
      std::ostringstream usage;
      usage << "video-compare " << VersionInfo::version << " " << VersionInfo::copyright << std::endl << std::endl;
      usage << "Usage: " << argv[0] << " [OPTIONS]... FILE1 FILE2" << std::endl << std::endl;
      argagg::fmt_ostream fmt(std::cerr);
      fmt << usage.str() << argparser;
    } else {
      VideoCompareConfig config;

      if (args.pos.size() != 2) {
        throw std::logic_error{"Two FFmpeg compatible video files must be supplied"};
      }

      config.verbose = args["verbose"];
      config.fit_window_to_usable_bounds = args["window-fit-display"];
      config.high_dpi_allowed = args["high-dpi"];
      config.use_10_bpc = args["10-bpc"];
      config.disable_auto_filters = args["disable-auto-filters"];

      if (args["display-number"]) {
        const std::string display_number_arg = args["display-number"];
        const std::regex display_number_re("(\\d*)");

        if (!std::regex_match(display_number_arg, display_number_re)) {
          throw std::logic_error{"Cannot parse display number argument (required format: [number], e.g. 0, 1 or 2)"};
        }

        config.display_number = std::stoi(display_number_arg);
      }
      if (args["display-mode"]) {
        const std::string display_mode_arg = args["display-mode"];

        if (display_mode_arg == "split") {
          config.display_mode = Display::Mode::split;
        } else if (display_mode_arg == "vstack") {
          config.display_mode = Display::Mode::vstack;
        } else if (display_mode_arg == "hstack") {
          config.display_mode = Display::Mode::hstack;
        } else {
          throw std::logic_error{"Cannot parse display mode argument (valid options: split, vstack, hstack)"};
        }
      }
      if (args["window-size"]) {
        if (config.fit_window_to_usable_bounds) {
          throw std::logic_error{"Options --window-size and --window-fit-display cannot be used together"};
        }

        const std::string window_size_arg = args["window-size"];
        const std::regex window_size_re("(\\d*)x(\\d*)");

        if (!std::regex_match(window_size_arg, window_size_re)) {
          throw std::logic_error{"Cannot parse window size argument (required format: [width]x[height], e.g. 800x600, 1280x or x480)"};
        }

        const std::regex delimiter_re("x");

        auto const token_vec = std::vector<std::string>(std::sregex_token_iterator{begin(window_size_arg), end(window_size_arg), delimiter_re, -1}, std::sregex_token_iterator{});

        config.window_size = std::make_tuple(!token_vec[0].empty() ? std::stoi(token_vec[0]) : -1, token_vec.size() == 2 ? std::stoi(token_vec[1]) : -1);
      }

      if (args["auto-loop-mode"]) {
        const std::string auto_loop_mode_arg = args["auto-loop-mode"];

        if (auto_loop_mode_arg == "off") {
          config.auto_loop_mode = Display::Loop::off;
        } else if (auto_loop_mode_arg == "on") {
          config.auto_loop_mode = Display::Loop::forwardonly;
        } else if (auto_loop_mode_arg == "pp") {
          config.auto_loop_mode = Display::Loop::pingpong;
        } else {
          throw std::logic_error{"Cannot parse auto loop mode argument (valid options: off, on, pp)"};
        }
      }
      if (args["frame-buffer-size"]) {
        const std::string frame_buffer_size_arg = args["frame-buffer-size"];
        const std::regex frame_buffer_size_re("(\\d*)");

        if (!std::regex_match(frame_buffer_size_arg, frame_buffer_size_re)) {
          throw std::logic_error{"Cannot parse frame buffer size (required format: [number], e.g. 10, 70 or 150)"};
        }

        config.frame_buffer_size = std::stoi(frame_buffer_size_arg);

        if (config.frame_buffer_size < 1) {
          throw std::logic_error{"Frame buffer size must be at least 1"};
        }
      }
      if (args["time-shift"]) {
        const std::string time_shift_arg = args["time-shift"];
        const std::regex time_shift_re("^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)$");

        if (!std::regex_match(time_shift_arg, time_shift_re)) {
          throw std::logic_error{"Cannot parse time shift argument; must be a valid number in seconds, e.g. 1 or -0.333"};
        }

        config.time_shift_ms = std::stod(time_shift_arg) * 1000.0;
      }
      if (args["wheel-sensitivity"]) {
        const std::string wheel_sensitivity_arg = args["wheel-sensitivity"];
        const std::regex wheel_sensitivity_re("^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)$");

        if (!std::regex_match(wheel_sensitivity_arg, wheel_sensitivity_re)) {
          throw std::logic_error{"Cannot parse mouse wheel sensitivity argument; must be a valid number, e.g. 1.3 or -1"};
        }

        config.wheel_sensitivity = std::stod(wheel_sensitivity_arg);
      }
      if (args["tone-map-mode"]) {
        const std::string tone_mapping_mode_arg = args["tone-map-mode"];

        if (tone_mapping_mode_arg == "off") {
          config.tone_mapping_mode = ToneMapping::off;
        } else if (tone_mapping_mode_arg == "on") {
          config.tone_mapping_mode = ToneMapping::fullrange;
        } else if (tone_mapping_mode_arg == "rel") {
          config.tone_mapping_mode = ToneMapping::relative;
        } else {
          throw std::logic_error{"Cannot parse tone mapping mode argument (valid options: off, on, rel)"};
        }
      }
      if (args["left-filters"]) {
        config.left.video_filters = static_cast<const std::string&>(args["left-filters"]);
      }
      if (args["right-filters"]) {
        config.right.video_filters = static_cast<const std::string&>(args["right-filters"]);
      }

      config.left.demuxer_options = create_default_demuxer_options();
      config.right.demuxer_options = create_default_demuxer_options();

      if (args["left-demuxer"]) {
        auto left_demuxer = static_cast<const std::string&>(args["left-demuxer"]);

        config.left.demuxer = get_nth_token_or_empty(left_demuxer, ':', 0);
        config.left.demuxer_options = upsert_avdict_options(config.left.demuxer_options, get_nth_token_or_empty(left_demuxer, ':', 1));
      }
      if (args["right-demuxer"]) {
        auto right_demuxer = static_cast<const std::string&>(args["right-demuxer"]);

        config.right.demuxer = get_nth_token_or_empty(right_demuxer, ':', 0);
        config.right.demuxer_options = upsert_avdict_options(config.right.demuxer_options, get_nth_token_or_empty(right_demuxer, ':', 1));
      }
      if (args["left-decoder"]) {
        auto left_decoder = static_cast<const std::string&>(args["left-decoder"]);

        config.left.decoder = get_nth_token_or_empty(left_decoder, ':', 0);
        config.left.decoder_options = upsert_avdict_options(nullptr, get_nth_token_or_empty(left_decoder, ':', 1));
      }
      if (args["right-decoder"]) {
        auto right_decoder = static_cast<const std::string&>(args["right-decoder"]);

        config.right.decoder = get_nth_token_or_empty(right_decoder, ':', 0);
        config.right.decoder_options = upsert_avdict_options(nullptr, get_nth_token_or_empty(right_decoder, ':', 1));
      }
      if (args["left-hwaccel"]) {
        auto left_hw_accel_spec = static_cast<const std::string&>(args["left-hwaccel"]);

        config.left.hw_accel_spec = string_join({get_nth_token_or_empty(left_hw_accel_spec, ':', 0), get_nth_token_or_empty(left_hw_accel_spec, ':', 1)}, ":");
        config.left.hw_accel_options = upsert_avdict_options(nullptr, get_nth_token_or_empty(left_hw_accel_spec, ':', 2));
      }
      if (args["right-hwaccel"]) {
        auto right_hw_accel_spec = static_cast<const std::string&>(args["right-hwaccel"]);

        config.right.hw_accel_spec = string_join({get_nth_token_or_empty(right_hw_accel_spec, ':', 0), get_nth_token_or_empty(right_hw_accel_spec, ':', 1)}, ":");
        config.right.hw_accel_options = upsert_avdict_options(nullptr, get_nth_token_or_empty(right_hw_accel_spec, ':', 2));
      }
      if (args["left-peak-nits"] || args["right-peak-nits"]) {
        const std::regex peak_nits_re("(\\d*)");

        auto parse_peak_nits = [&](const std::string& arg, const std::string& location) {
          if (!std::regex_match(arg, peak_nits_re)) {
            throw std::logic_error{"Cannot parse " + to_lower_case(location) + " peak nits (required format: [number], e.g. 400, 850 or 1000)"};
          }

          int result = std::stoi(arg);

          if (result < 1) {
            throw std::logic_error{location + " peak nits must be at least 1"};
          }
          if (result > 10000) {
            throw std::logic_error{location + " peak nits must not be more than 10000"};
          }
          return result;
        };

        if (args["left-peak-nits"]) {
          config.left.peak_luminance_nits = parse_peak_nits(args["left-peak-nits"], "Left");
        }
        if (args["right-peak-nits"]) {
          config.right.peak_luminance_nits = parse_peak_nits(args["right-peak-nits"], "Right");
        }

        // enable tone mapping if option wasn't specified
        if (!args["tone-map-mode"]) {
          config.tone_mapping_mode = ToneMapping::fullrange;
        }
      }
      if (args["boost-tone"]) {
        const std::string boost_tone_arg = args["boost-tone"];
        const std::regex boost_tone_re("^([0-9]+([.][0-9]*)?|[.][0-9]+)$");

        if (!std::regex_match(boost_tone_arg, boost_tone_re)) {
          throw std::logic_error{"Cannot parse boost luminance argument; must be a valid number, e.g. 1.3 or 3.0"};
        }

        config.boost_tone = std::stod(boost_tone_arg);

        // enable tone mapping if option wasn't specified
        if (!args["tone-map-mode"]) {
          config.tone_mapping_mode = ToneMapping::fullrange;
        }
      }

      config.left.file_name = args.pos[0];
      config.right.file_name = args.pos[1];

      if (config.left.file_name == REPEAT_FILE_NAME && config.right.file_name == REPEAT_FILE_NAME) {
        throw std::logic_error{"At least one actual video file must be supplied"};
      } else if (config.left.file_name == REPEAT_FILE_NAME) {
        config.left.file_name = config.right.file_name;
      } else if (config.right.file_name == REPEAT_FILE_NAME) {
        config.right.file_name = config.left.file_name;
      }

      if (args["libvmaf-options"]) {
        VMAFCalculator::instance().set_libvmaf_options(args["libvmaf-options"]);
      }

      VideoCompare compare{config};
      compare();
    }
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    exit_code = -1;
  }

  free_argv(argc, argv_decoded);

  return exit_code;
}
