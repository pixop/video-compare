#define SDL_MAIN_HANDLED
#include <algorithm>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <vector>
#include "argagg.h"
#include "controls.h"
#include "side_aware_logger.h"
#include "string_utils.h"
#include "version.h"
#include "video_compare.h"
#include "vmaf_calculator.h"

#ifdef _WIN32
#include <Windows.h>
#undef RELATIVE

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

TimeShiftConfig parse_time_shift(const std::string& time_shift_arg) {
  TimeShiftConfig config;

  // Check if it's a simple number or time format, treat it as an offset
  try {
    const double offset = parse_timestamps_to_seconds(time_shift_arg);
    config.offset_ms = static_cast<int64_t>(offset * 1000.0);
    return config;
  } catch (const std::logic_error& e) {
    // If parsing as time format fails, continue with multiplier parsing
  }

  // Parse new format: [xmultiplier][+/-offset]
  std::string remaining = time_shift_arg;

  // Parse multiplier if present
  if (remaining[0] == 'x') {
    remaining = remaining.substr(1);  // Remove 'x'

    // Find the end of the multiplier (either end of string or start of offset)
    size_t multiplier_end = remaining.find_first_of("+-");
    if (multiplier_end == std::string::npos) {
      multiplier_end = remaining.length();
    }

    const std::string multiplier_str = remaining.substr(0, multiplier_end);
    remaining = remaining.substr(multiplier_end);

    // Check if it's a rational fraction (e.g., "25/24")
    const std::regex number_re("^([0-9]+([.][0-9]*)?|[.][0-9]+)$");

    size_t slash_pos = multiplier_str.find('/');
    if (slash_pos != std::string::npos) {
      std::string numerator_str = multiplier_str.substr(0, slash_pos);
      std::string denominator_str = multiplier_str.substr(slash_pos + 1);

      if (!std::regex_match(numerator_str, number_re) || !std::regex_match(denominator_str, number_re)) {
        throw std::logic_error{"Cannot parse time shift multiplier; numerator and denominator must be valid postive numbers"};
      }

      const double numerator = parse_strict_double(numerator_str);
      const double denominator = parse_strict_double(denominator_str);

      if (denominator == 0) {
        throw std::logic_error{"Cannot parse time shift multiplier; denominator cannot be zero"};
      }

      av_reduce(&config.multiplier.num, &config.multiplier.den, std::round(numerator * 10000), std::round(denominator * 10000), 1000000);
    } else {
      if (!std::regex_match(multiplier_str, number_re)) {
        throw std::logic_error{"Cannot parse time shift multiplier; must be a valid positive number"};
      }

      config.multiplier = av_d2q(parse_strict_double(multiplier_str), 1000000);
    }

    // Prevent division by zero in inverse multiplier
    if (config.multiplier.num == 0) {
      throw std::runtime_error("Multiplier cannot be zero");
    }
  }

  // Parse offset if present
  if (!remaining.empty()) {
    try {
      double offset = parse_timestamps_to_seconds(remaining);
      config.offset_ms = static_cast<int64_t>(offset * 1000.0);
    } catch (const std::logic_error& e) {
      throw std::logic_error{"Cannot parse time shift offset: " + std::string(e.what())};
    }
  }

  return config;
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

static const std::string PLACEHOLDER("__");
static const std::regex PLACEHOLDER_REGEX(PLACEHOLDER);

static inline bool contains_placeholder(const std::string& str) {
  return str.find(PLACEHOLDER) != std::string::npos;
}

std::string safe_replace_placeholder(const std::string& template_str, const std::string& replacement, const std::string& type) {
  if (contains_placeholder(template_str) && contains_placeholder(replacement)) {
    throw std::logic_error{"Unable to replace placeholder in " + type + ": replacement contains an unresolved placeholder."};
  }

  return replacement.empty() ? template_str : std::regex_replace(template_str, PLACEHOLDER_REGEX, replacement, std::regex_constants::format_first_only);
}

void resolve_mutual_placeholders(std::string& left, std::string& right, const std::string& type, bool only_replace_full_placeholder = false) {
  if ((contains_placeholder(left) && right.empty()) || (left.empty() && contains_placeholder(right))) {
    throw std::logic_error{"Cannot resolve placeholder in " + type + ": the other is empty and cannot be substituted."};
  }

  if (only_replace_full_placeholder) {
    if ((left == PLACEHOLDER) && (right == PLACEHOLDER)) {
      throw std::logic_error{"Cannot resolve placeholder in " + type + ": the other is also a placeholder."};
    } else if (left == PLACEHOLDER) {
      left = right;
    } else if (right == PLACEHOLDER) {
      right = left;
    }
  } else {
    if (contains_placeholder(left)) {
      left = safe_replace_placeholder(left, right, type);
    } else if (contains_placeholder(right)) {
      right = safe_replace_placeholder(right, left, type);
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
    argagg::parser argparser{
        {{"help", {"-h", "--help"}, "show help", 0},
         {"show-controls", {"-c", "--show-controls"}, "show controls", 0},
         {"verbose", {"-v", "--verbose"}, "enable verbose output, including information such as library versions and rendering details", 0},
         {"high-dpi", {"-d", "--high-dpi"}, "allow high DPI mode for e.g. displaying UHD content on Retina displays", 0},
         {"10-bpc", {"-b", "--10-bpc"}, "use 10 bits per color component instead of 8", 0},
         {"fast-alignment", {"-F", "--fast-alignment"}, "toggle fast bilinear scaling for aligning input source resolutions, replacing high-quality bicubic and chroma-accurate interpolation", 0},
         {"bilinear-texture", {"-I", "--bilinear-texture"}, "toggle bilinear video texture interpolation, replacing nearest-neighbor filtering", 0},
         {"display-number", {"-n", "--display-number"}, "open main window on specific display (e.g. 0, 1 or 2), default is 0", 1},
         {"display-mode", {"-m", "--mode"}, "display mode (layout), 'split' for split screen (default), 'vstack' for vertical stack, 'hstack' for horizontal stack", 1},
         {"window-size", {"-w", "--window-size"}, "override window size, specified as [width]x[height] (e.g. 800x600, 1280x or x480)", 1},
         {"window-fit-display", {"-W", "--window-fit-display"}, "calculate the window size to fit within the usable display bounds while maintaining the video aspect ratio", 0},
         {"auto-loop-mode", {"-a", "--auto-loop-mode"}, "auto-loop playback when buffer fills, 'off' for continuous streaming (default), 'on' for forward-only mode, 'pp' for ping-pong mode", 1},
         {"frame-buffer-size", {"-f", "--frame-buffer-size"}, "frame buffer size (e.g. 10, 70 or 150), default is 50", 1},
         {"time-shift", {"-t", "--time-shift"}, "shift the time stamps of the right video by a user-specified time offset, optionally with a multiplier (e.g. 0.150, -0.1, x1.04+0.1, x25.025/24-1:30.5)", 1},
         {"wheel-sensitivity", {"-s", "--wheel-sensitivity"}, "mouse wheel sensitivity (e.g. 0.5, -1 or 1.7), default is 1; negative values invert the input direction", 1},
         {"color-space", {"-C", "--color-space"}, "set the color space matrix, specified as [matrix] for the same on both sides, or [l-matrix?]:[r-matrix?] for different values (e.g. 'bt709' or 'bt2020nc:')", 1},
         {"color-range", {"-A", "--color-range"}, "set the color range, specified as [range] for the same on both sides, or [l-range?]:[r-range?] for different values (e.g. 'tv', ':pc' or 'pc:tv')", 1},
         {"color-primaries", {"-P", "--color-primaries"}, "set the color primaries, specified as [primaries] for the same on both sides, or [l-primaries?]:[r-primaries?] for different values (e.g. 'bt709' or 'bt2020:bt709')", 1},
         {"color-trc", {"-N", "--color-trc"}, "set the transfer characteristics (transfer curve), specified as [trc] for the same on both sides, or [l-trc?]:[r-trc?] for different values (e.g. 'bt709' or 'smpte2084:')", 1},
         {"tone-map-mode", {"-T", "--tone-map-mode"}, "adapt tones for sRGB display: 'auto' (default) for automatic HDR, 'off' for none, 'on' for full-range mapping, 'rel' for relative comparison (e.g. 'on', 'auto:off', ':rel')", 1},
         {"left-peak-nits", {"-L", "--left-peak-nits"}, "left video peak luminance in nits (e.g. 850 or 1000), default is 100 for SDR and 500 for HDR", 1},
         {"right-peak-nits", {"-R", "--right-peak-nits"}, "right video peak luminance in nits; see --left-peak-nits", 1},
         {"boost-tone", {"-B", "--boost-tone"}, "adjust tone-mapping strength factor, specified as [factor] for the same on both sides, or [l-factor?]:[r-factor?] for different values (e.g. '0.6', ':3' or '2:1.5')", 1},
         {"filters", {"-i", "--filters"}, "specify a comma-separated list of FFmpeg filters to be applied to both sides (e.g. scale=1920:-2,delogo=x=10:y=10:w=100:h=70)", 1},
         {"left-filters", {"-l", "--left-filters"}, "specify a comma-separated list of FFmpeg filters to be applied to the left video (e.g. format=gray,crop=iw:ih-240)", 1},
         {"right-filters", {"-r", "--right-filters"}, "specify a comma-separated list of FFmpeg filters to be applied to the right video (e.g. yadif,hqdn3d,pad=iw+320:ih:160:0)", 1},
         {"find-filters", {"--find-filters"}, "find FFmpeg video filters that match the provided search term (e.g. 'scale', 'libvmaf' or 'dnn'; use \"\" to list all)", 1},
         {"find-protocols", {"--find-protocols"}, "find FFmpeg input protocols that match the provided search term (e.g. 'ipfs', 'srt', or 'rtmp'; use \"\" to list all)", 1},
         {"demuxer", {"--demuxer"}, "left FFmpeg video demuxer name for both sides, specified as [type?][:options?] (e.g. 'rawvideo:pixel_format=rgb24,video_size=320x240,framerate=10')", 1},
         {"left-demuxer", {"--left-demuxer"}, "left FFmpeg video demuxer name, specified as [type?][:options?]", 1},
         {"right-demuxer", {"--right-demuxer"}, "right FFmpeg video demuxer name, specified as [type?][:options?]", 1},
         {"find-demuxers", {"--find-demuxers"}, "find FFmpeg video demuxers that match the provided search term (e.g. 'matroska', 'mp4', 'vapoursynth' or 'pipe'; use \"\" to list all)", 1},
         {"decoder", {"--decoder"}, "FFmpeg video decoder name for both sides, specified as [type?][:options?] (e.g. ':strict=unofficial', ':strict=-2' or 'vvc:strict=experimental')", 1},
         {"left-decoder", {"--left-decoder"}, "left FFmpeg video decoder name, specified as [type?][:options?] (e.g. ':strict=-2,trust_dec_pts=1' or 'h264:trust_dec_pts=1')", 1},
         {"right-decoder", {"--right-decoder"}, "right FFmpeg video decoder name, specified as [type?][:options?]", 1},
         {"find-decoders", {"--find-decoders"}, "find FFmpeg video decoders that match the provided search term (e.g. 'h264', 'hevc', 'av1' or 'cuvid'; use \"\" to list all)", 1},
         {"hwaccel", {"--hwaccel"}, "FFmpeg video hardware acceleration for both sides, specified as [type][:device?[:options?]] (e.g. 'videotoolbox' or 'vaapi:/dev/dri/renderD128')", 1},
         {"left-hwaccel", {"--left-hwaccel"}, "left FFmpeg video hardware acceleration, specified as [type][:device?[:options?]] (e.g. 'cuda', 'cuda:1' or 'vulkan')", 1},
         {"right-hwaccel", {"--right-hwaccel"}, "right FFmpeg video hardware acceleration, specified as [type][:device?[:options?]]", 1},
         {"find-hwaccels", {"--find-hwaccels"}, "find FFmpeg video hardware acceleration types that match the provided search term (e.g. 'videotoolbox' or 'vulkan'; use \"\" to list all)", 1},
         {"libvmaf-options", {"--libvmaf-options"}, "libvmaf FFmpeg filter options (e.g. 'model=version=vmaf_4k_v0.6.1' or 'model=version=vmaf_v0.6.1\\\\:name=hd|version=vmaf_4k_v0.6.1\\\\:name=4k')", 1},
         {"disable-auto-filters", {"--no-auto-filters"}, "disable the default behaviour of automatically injecting filters for deinterlacing, DAR correction, frame rate harmonization, rotation and colorimetry", 0}}};

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
      config.fast_input_alignment = args["fast-alignment"];
      config.bilinear_texture_filtering = args["bilinear-texture"];
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
          config.display_mode = Display::Mode::SPLIT;
        } else if (display_mode_arg == "vstack") {
          config.display_mode = Display::Mode::VSTACK;
        } else if (display_mode_arg == "hstack") {
          config.display_mode = Display::Mode::HSTACK;
        } else {
          throw std::logic_error{"Cannot parse display mode argument (valid options: split, vstack, hstack)"};
        }
      }
      if (args["color-space"]) {
        auto color_space_spec = static_cast<const std::string&>(args["color-space"]);
        auto left_color_space = get_nth_token_or_empty(color_space_spec, ':', 0);

        config.left.color_space = left_color_space;
        config.right.color_space = (color_space_spec == left_color_space) ? color_space_spec : get_nth_token_or_empty(color_space_spec, ':', 1);
      }
      if (args["color-range"]) {
        auto color_range_spec = static_cast<const std::string&>(args["color-range"]);
        auto left_color_range = get_nth_token_or_empty(color_range_spec, ':', 0);

        config.left.color_range = left_color_range;
        config.right.color_range = (color_range_spec == left_color_range) ? color_range_spec : get_nth_token_or_empty(color_range_spec, ':', 1);
      }
      if (args["color-primaries"]) {
        auto color_primaries_spec = static_cast<const std::string&>(args["color-primaries"]);
        auto left_primaries = get_nth_token_or_empty(color_primaries_spec, ':', 0);

        config.left.color_primaries = left_primaries;
        config.right.color_primaries = (color_primaries_spec == left_primaries) ? color_primaries_spec : get_nth_token_or_empty(color_primaries_spec, ':', 1);
      }
      if (args["color-trc"]) {
        auto color_trc_spec = static_cast<const std::string&>(args["color-trc"]);
        auto left_trc = get_nth_token_or_empty(color_trc_spec, ':', 0);

        config.left.color_trc = left_trc;
        config.right.color_trc = (color_trc_spec == left_trc) ? color_trc_spec : get_nth_token_or_empty(color_trc_spec, ':', 1);
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
          config.auto_loop_mode = Display::Loop::OFF;
        } else if (auto_loop_mode_arg == "on") {
          config.auto_loop_mode = Display::Loop::FORWARDONLY;
        } else if (auto_loop_mode_arg == "pp") {
          config.auto_loop_mode = Display::Loop::PINGPONG;
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

        try {
          const TimeShiftConfig time_shift_config = parse_time_shift(time_shift_arg);
          config.time_shift = time_shift_config;

          const double multiplier_value = av_q2d(time_shift_config.multiplier);
          std::cout << string_sprintf("Timeshift config: multiplier=%d/%d (x%.6f), offset=%ld ms", time_shift_config.multiplier.num, time_shift_config.multiplier.den, multiplier_value, time_shift_config.offset_ms) << std::endl;
        } catch (const std::logic_error& e) {
          throw std::logic_error{"Cannot parse time shift argument: " + std::string(e.what())};
        }
      }
      if (args["wheel-sensitivity"]) {
        const std::string wheel_sensitivity_arg = args["wheel-sensitivity"];
        const std::regex wheel_sensitivity_re("^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)$");

        if (!std::regex_match(wheel_sensitivity_arg, wheel_sensitivity_re)) {
          throw std::logic_error{"Cannot parse mouse wheel sensitivity argument; must be a valid number, e.g. 1.3 or -1"};
        }

        config.wheel_sensitivity = parse_strict_double(wheel_sensitivity_arg);
      }
      if (args["tone-map-mode"]) {
        const std::string tone_mapping_mode_arg = args["tone-map-mode"];

        auto parse_tm_mode_arg = [](const std::string& tone_mapping_mode_arg) {
          if (tone_mapping_mode_arg.empty() || tone_mapping_mode_arg == "auto") {
            return ToneMapping::AUTO;
          } else if (tone_mapping_mode_arg == "off") {
            return ToneMapping::OFF;
          } else if (tone_mapping_mode_arg == "on") {
            return ToneMapping::FULLRANGE;
          } else if (tone_mapping_mode_arg == "rel") {
            return ToneMapping::RELATIVE;
          } else {
            throw std::logic_error{"Cannot parse tone mapping mode argument (valid options: auto, off, on, rel)"};
          }
        };

        auto tone_mapping_mode_spec = static_cast<const std::string&>(args["tone-map-mode"]);
        auto left_tone_mapping_mode = get_nth_token_or_empty(tone_mapping_mode_spec, ':', 0);

        config.left.tone_mapping_mode = parse_tm_mode_arg(left_tone_mapping_mode);
        config.right.tone_mapping_mode = (tone_mapping_mode_spec == left_tone_mapping_mode) ? config.left.tone_mapping_mode : parse_tm_mode_arg(get_nth_token_or_empty(tone_mapping_mode_spec, ':', 1));
      }

      // video filters
      if (args["filters"]) {
        config.left.video_filters = static_cast<const std::string&>(args["filters"]);
        config.right.video_filters = static_cast<const std::string&>(args["filters"]);
      }
      if (args["left-filters"]) {
        config.left.video_filters = safe_replace_placeholder(static_cast<const std::string&>(args["left-filters"]), config.left.video_filters, "filter specification");
      }
      if (args["right-filters"]) {
        config.right.video_filters = safe_replace_placeholder(static_cast<const std::string&>(args["right-filters"]), config.right.video_filters, "filter specification");
      }
      resolve_mutual_placeholders(config.left.video_filters, config.right.video_filters, "filter specification");

      // demuxer
      config.left.demuxer_options = create_default_demuxer_options();
      config.right.demuxer_options = create_default_demuxer_options();

      if (args["demuxer"]) {
        config.left.demuxer = static_cast<const std::string&>(args["demuxer"]);
        config.right.demuxer = static_cast<const std::string&>(args["demuxer"]);
      }
      if (args["left-demuxer"]) {
        config.left.demuxer = safe_replace_placeholder(static_cast<const std::string&>(args["left-demuxer"]), config.left.demuxer, "demuxer");
      }
      if (args["right-demuxer"]) {
        config.right.demuxer = safe_replace_placeholder(static_cast<const std::string&>(args["right-demuxer"]), config.right.demuxer, "demuxer");
      }
      resolve_mutual_placeholders(config.left.demuxer, config.right.demuxer, "demuxer");

      config.left.demuxer_options = upsert_avdict_options(config.left.demuxer_options, get_nth_token_or_empty(config.left.demuxer, ':', 1));
      config.right.demuxer_options = upsert_avdict_options(config.right.demuxer_options, get_nth_token_or_empty(config.right.demuxer, ':', 1));
      config.left.demuxer = get_nth_token_or_empty(config.left.demuxer, ':', 0);
      config.right.demuxer = get_nth_token_or_empty(config.right.demuxer, ':', 0);

      // decder
      if (args["decoder"]) {
        config.left.decoder = static_cast<const std::string&>(args["decoder"]);
        config.right.decoder = static_cast<const std::string&>(args["decoder"]);
      }
      if (args["left-decoder"]) {
        config.left.decoder = safe_replace_placeholder(static_cast<const std::string&>(args["left-decoder"]), config.left.decoder, "decoder");
      }
      if (args["right-decoder"]) {
        config.right.decoder = safe_replace_placeholder(static_cast<const std::string&>(args["right-decoder"]), config.right.decoder, "decoder");
      }
      resolve_mutual_placeholders(config.left.decoder, config.right.decoder, "decoder");

      config.left.decoder_options = upsert_avdict_options(nullptr, get_nth_token_or_empty(config.left.decoder, ':', 1));
      config.right.decoder_options = upsert_avdict_options(nullptr, get_nth_token_or_empty(config.right.decoder, ':', 1));
      config.left.decoder = get_nth_token_or_empty(config.left.decoder, ':', 0);
      config.right.decoder = get_nth_token_or_empty(config.right.decoder, ':', 0);

      // HW acceleration
      if (args["hwaccel"]) {
        config.left.hw_accel_spec = static_cast<const std::string&>(args["hwaccel"]);
        config.right.hw_accel_spec = static_cast<const std::string&>(args["hwaccel"]);
      }
      if (args["left-hwaccel"]) {
        config.left.hw_accel_spec = safe_replace_placeholder(static_cast<const std::string&>(args["left-hwaccel"]), config.left.hw_accel_spec, "hardware acceleration");
      }
      if (args["right-hwaccel"]) {
        config.right.hw_accel_spec = safe_replace_placeholder(static_cast<const std::string&>(args["right-hwaccel"]), config.right.hw_accel_spec, "hardware acceleration");
      }
      resolve_mutual_placeholders(config.left.hw_accel_spec, config.right.hw_accel_spec, "hardware acceleration");

      config.left.hw_accel_options = upsert_avdict_options(nullptr, get_nth_token_or_empty(config.left.hw_accel_spec, ':', 2));
      config.right.hw_accel_options = upsert_avdict_options(nullptr, get_nth_token_or_empty(config.right.hw_accel_spec, ':', 2));
      config.left.hw_accel_spec = string_join({get_nth_token_or_empty(config.left.hw_accel_spec, ':', 0), get_nth_token_or_empty(config.left.hw_accel_spec, ':', 1)}, ":");
      config.right.hw_accel_spec = string_join({get_nth_token_or_empty(config.right.hw_accel_spec, ':', 0), get_nth_token_or_empty(config.right.hw_accel_spec, ':', 1)}, ":");

      if (args["left-peak-nits"] || args["right-peak-nits"]) {
        const std::regex peak_nits_re("(\\d*)");

        auto parse_peak_nits = [&](const std::string& arg, const InputVideo& input_video) {
          if (!std::regex_match(arg, peak_nits_re)) {
            throw std::logic_error{"Cannot parse " + to_lower_case(input_video.side_description) + " peak nits (required format: [number], e.g. 400, 850 or 1000)"};
          }

          int result = std::stoi(arg);

          if (result < 1) {
            throw std::logic_error{input_video.side_description + " peak nits must be at least 1"};
          }
          if (result > 10000) {
            throw std::logic_error{input_video.side_description + " peak nits must not be more than 10000"};
          }
          return result;
        };

        std::string left_peak_nits;
        std::string right_peak_nits;

        if (args["left-peak-nits"]) {
          left_peak_nits = static_cast<const std::string&>(args["left-peak-nits"]);
        }
        if (args["right-peak-nits"]) {
          right_peak_nits = static_cast<const std::string&>(args["right-peak-nits"]);
        }
        resolve_mutual_placeholders(left_peak_nits, right_peak_nits, "peak (in nits)");

        if (!left_peak_nits.empty()) {
          config.left.peak_luminance_nits = parse_peak_nits(left_peak_nits, config.left);
        }
        if (!right_peak_nits.empty()) {
          config.right.peak_luminance_nits = parse_peak_nits(right_peak_nits, config.right);
        }
      }
      if (args["boost-tone"]) {
        auto parse_boost_tone = [](const std::string& boost_tone_arg, const InputVideo& input_video) {
          if (boost_tone_arg.empty()) {
            return 1.0;
          }

          const std::regex boost_tone_re("^([0-9]+([.][0-9]*)?|[.][0-9]+)$");

          if (!std::regex_match(boost_tone_arg, boost_tone_re)) {
            throw std::logic_error{"Cannot parse " + to_lower_case(input_video.side_description) + " boost luminance argument; must be a valid number, e.g. 1.3 or 3.0"};
          }

          return parse_strict_double(boost_tone_arg);
        };

        auto boost_tone_spec = static_cast<const std::string&>(args["boost-tone"]);
        auto left_boost_tone = get_nth_token_or_empty(boost_tone_spec, ':', 0);

        config.left.boost_tone = parse_boost_tone(left_boost_tone, config.left);
        config.right.boost_tone = (boost_tone_spec == left_boost_tone) ? config.left.boost_tone : parse_boost_tone(get_nth_token_or_empty(boost_tone_spec, ':', 1), config.right);
      }

      config.left.file_name = args.pos[0];
      config.right.file_name = args.pos[1];

      resolve_mutual_placeholders(config.left.file_name, config.right.file_name, "video file", true);

      if (args["libvmaf-options"]) {
        VMAFCalculator::instance().set_libvmaf_options(args["libvmaf-options"]);
      }

      av_log_set_callback(sa_av_log_callback);

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
