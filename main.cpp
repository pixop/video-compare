#define SDL_MAIN_HANDLED
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
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

static const std::string AUTO_OPTIONS_FILE_NAME = "video-compare.opt";

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

std::vector<std::string> read_options_file(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::logic_error{"Cannot open options file: " + path};
  }

  std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return tokenize_command_line_options(contents);
}

std::vector<std::string> read_auto_options_file_if_present(bool enabled) {
  if (!enabled) {
    return {};
  }

  std::ifstream probe(AUTO_OPTIONS_FILE_NAME);
  if (!probe) {
    return {};
  }

  return read_options_file(AUTO_OPTIONS_FILE_NAME);
}

std::vector<std::string> expand_options_files(const argagg::option_results& results) {
  std::vector<std::string> extra_args;

  for (const auto& result : results.all) {
    if (!result) {
      throw std::logic_error{"Missing file path after --options-file"};
    }

    auto file_args = read_options_file(result.arg);
    extra_args.insert(extra_args.end(), file_args.begin(), file_args.end());
  }

  return extra_args;
}

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

ToneMapping parse_tone_mapping_mode(const std::string& mode) {
  if (mode.empty() || mode == "auto") {
    return ToneMapping::Auto;
  } else if (mode == "off") {
    return ToneMapping::Off;
  } else if (mode == "on") {
    return ToneMapping::FullRange;
  } else if (mode == "rel") {
    return ToneMapping::Relative;
  } else {
    throw std::logic_error{"Cannot parse tone mapping mode (valid options: auto, off, on, rel)"};
  }
}

unsigned parse_peak_nits(const std::string& nits_str) {
  if (nits_str.empty()) {
    return 0;
  }
  const std::regex peak_nits_re("(\\d*)");
  if (!std::regex_match(nits_str, peak_nits_re)) {
    throw std::logic_error{"Cannot parse peak nits (required format: [number], e.g. 400, 850 or 1000)"};
  }
  int nits = std::stoi(nits_str);
  if (nits < 1) {
    throw std::logic_error{"peak nits must be at least 1"};
  }
  if (nits > 10000) {
    throw std::logic_error{"peak nits must not be more than 10000"};
  }
  return static_cast<unsigned>(nits);
}

float parse_boost_tone(const std::string& boost_str) {
  if (boost_str.empty()) {
    return 1.0;
  }
  const std::regex boost_tone_re("^([0-9]+([.][0-9]*)?|[.][0-9]+)$");
  if (!std::regex_match(boost_str, boost_tone_re)) {
    throw std::logic_error{"Cannot parse boost tone; must be a valid number, e.g. 1.3 or 3.0"};
  }
  return static_cast<float>(parse_strict_double(boost_str));
}

// Parse an FFmpeg parameter spec string (format: "name[:options]" or "name:device:options" for hwaccel)
// Returns the main value and sets options in the provided AVDictionary
// For hwaccel with join_tokens_0_and_1=true, joins tokens 0 and 1 as the main value
std::string parse_ffmpeg_param_spec(const std::string& spec, const std::string& template_spec, AVDictionary*& options, const std::string& type_name, int options_token_idx, bool use_default_demuxer_opts, bool join_tokens_0_and_1) {
  std::string result = safe_replace_placeholder(spec, template_spec, type_name);
  AVDictionary* base_dict = options;
  if (!base_dict && use_default_demuxer_opts) {
    base_dict = create_default_demuxer_options();
  }
  options = upsert_avdict_options(base_dict, get_nth_token_or_empty(result, ':', options_token_idx));
  if (join_tokens_0_and_1) {
    return string_join({get_nth_token_or_empty(result, ':', 0), get_nth_token_or_empty(result, ':', 1)}, ":");
  } else {
    return get_nth_token_or_empty(result, ':', 0);
  }
}

// Parse right video specification with :: separator
// Format: filename[::key=value[::key=value...]]
struct RightVideoSpec {
  std::string file_name;
  std::map<std::string, std::string> params;
};

RightVideoSpec parse_right_video_spec(const std::string& spec) {
  RightVideoSpec result;
  size_t pos = spec.find("::");

  if (pos == std::string::npos) {
    result.file_name = spec;
    return result;
  }

  result.file_name = spec.substr(0, pos);
  size_t start = pos + 2;

  while (start < spec.length()) {
    size_t next_sep = spec.find("::", start);
    std::string part = (next_sep == std::string::npos) ? spec.substr(start) : spec.substr(start, next_sep - start);
    start = (next_sep == std::string::npos) ? spec.length() : next_sep + 2;

    size_t eq_pos = part.find('=');
    if (eq_pos != std::string::npos) {
      result.params[part.substr(0, eq_pos)] = part.substr(eq_pos + 1);
    } else if (!part.empty()) {
      result.params[part] = "";
    }
  }

  return result;
}

void apply_right_video_spec(InputVideo& video, const RightVideoSpec& spec, const InputVideo& template_video) {
  auto get_param = [&](const std::string& key) -> const std::string* {
    auto it = spec.params.find(key);
    return (it != spec.params.end()) ? &it->second : nullptr;
  };

  if (const std::string* val = get_param("filters")) {
    video.video_filters = safe_replace_placeholder(*val, template_video.video_filters, "filter specification");
  }
  if (const std::string* val = get_param("color-space")) {
    video.color_space = *val;
  }
  if (const std::string* val = get_param("color-range")) {
    video.color_range = *val;
  }
  if (const std::string* val = get_param("color-primaries")) {
    video.color_primaries = *val;
  }
  if (const std::string* val = get_param("color-trc")) {
    video.color_trc = *val;
  }

  if (const std::string* val = get_param("decoder")) {
    video.decoder = parse_ffmpeg_param_spec(*val, template_video.decoder, video.decoder_options, "decoder", 1, false, false);
  }
  if (const std::string* val = get_param("demuxer")) {
    video.demuxer = parse_ffmpeg_param_spec(*val, template_video.demuxer, video.demuxer_options, "demuxer", 1, true, false);
  }
  if (const std::string* val = get_param("hwaccel")) {
    video.hw_accel_spec = parse_ffmpeg_param_spec(*val, template_video.hw_accel_spec, video.hw_accel_options, "hardware acceleration", 2, false, true);
  }

  if (const std::string* val = get_param("tone-map-mode")) {
    video.tone_mapping_mode = parse_tone_mapping_mode(*val);
  }
  if (const std::string* val = get_param("peak-nits")) {
    video.peak_luminance_nits = parse_peak_nits(*val);
  }
  if (const std::string* val = get_param("boost-tone")) {
    video.boost_tone = parse_boost_tone(*val);
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
        {{"help", {"-h", "--help"}, "print help and exit", 0},
         {"show-controls", {"-c", "--show-controls"}, "print controls and exit", 0},
         {"verbose", {"-v", "--verbose"}, "enable verbose output, including information such as library versions and rendering details", 0},
         {"options-file", {"--options-file"}, "read additional command-line options from a file (contents are inserted before other arguments)", 1},
         {"no-auto-options-file", {"--no-auto-options-file"}, string_sprintf("do not read options from '%s' automatically", AUTO_OPTIONS_FILE_NAME.c_str()), 0},
         {"high-dpi", {"-d", "--high-dpi"}, "allow high DPI mode for e.g. displaying UHD content on Retina displays", 0},
         {"10-bpc", {"-b", "--10-bpc"}, "use 10 bits per color component instead of 8", 0},
         {"fast-alignment", {"-F", "--fast-alignment"}, "toggle fast bilinear scaling for aligning input source resolutions, replacing high-quality bicubic and chroma-accurate interpolation", 0},
         {"bilinear-texture", {"-I", "--bilinear-texture"}, "toggle bilinear video texture interpolation, replacing nearest-neighbor filtering", 0},
         {"subtraction-mode", {"-S", "--subtraction-mode"}, "start in subtraction (difference) view", 0},
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
         {"histogram-window", {"--histogram-window"}, "open always-on-top histogram scopes window", 0},
         {"vectorscope-window", {"--vectorscope-window"}, "open always-on-top vectorscope scopes window", 0},
         {"waveform-window", {"--waveform-window"}, "open always-on-top waveform scopes window", 0},
         {"histogram-options", {"--histogram-options"}, "histogram FFmpeg filter options (e.g. 'display_mode=parade:colors_mode=coloronblack:level_height=256:levels_mode=logarithmic')", 1},
         {"vectorscope-options", {"--vectorscope-options"}, "vectorscope FFmpeg filter options (e.g. 'mode=color4:graticule=green:envelope=instant+peak:flags=name+white+black')", 1},
         {"waveform-options", {"--waveform-options"}, "waveform FFmpeg filter options (e.g. 'graticule=orange:display=stack:scale=ire:flags=numbers+dots:intensity=0.1:components=7:filter=lowpass')", 1},
         {"scope-size", {"--scope-size"}, "set initial scope window size as WxH (total width by height); scope windows are resizable; default 1024x256", 1},
         {"scope-notop", {"--scope-notop"}, "do not keep scope windows always on top", 0},
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

    argagg::parser_results initial_args;
    initial_args = argparser.parse(argc, argv_decoded);

    const bool auto_options_enabled = !initial_args["no-auto-options-file"];
    std::vector<std::string> auto_options_args = read_auto_options_file_if_present(auto_options_enabled);
    std::vector<std::string> options_file_args = expand_options_files(initial_args["options-file"]);
    std::vector<std::string> merged_args;
    merged_args.reserve(static_cast<size_t>(argc) + auto_options_args.size() + options_file_args.size());
    merged_args.emplace_back(argv_decoded[0]);
    merged_args.insert(merged_args.end(), auto_options_args.begin(), auto_options_args.end());
    merged_args.insert(merged_args.end(), options_file_args.begin(), options_file_args.end());
    for (int i = 1; i < argc; ++i) {
      merged_args.emplace_back(argv_decoded[i]);
    }

    std::vector<char*> merged_argv;
    merged_argv.reserve(merged_args.size());
    for (auto& arg : merged_args) {
      merged_argv.push_back(const_cast<char*>(arg.c_str()));
    }

    argagg::parser_results args;
    args = argparser.parse(static_cast<int>(merged_argv.size()), merged_argv.data());
    if (args["verbose"]) {
      std::cout << "Command line: " << format_command_line_for_log(merged_args) << std::endl;
    }

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
      usage << "Usage: " << argv[0] << " [OPTIONS]... FILE1 FILE2 [FILE3] [FILE4] ..." << std::endl << std::endl;
      argagg::fmt_ostream fmt(std::cerr);
      fmt << usage.str() << argparser;
    } else {
      VideoCompareConfig config;

      if (args.pos.size() < 2) {
        throw std::logic_error{"Two or more FFmpeg compatible video files must be supplied (left and at least one right)"};
      }

      // Create a temporary right video for parsing command-line options
      // This will be used as a template for all right videos
      InputVideo right_template{RIGHT, "Right"};

      config.verbose = args["verbose"];
      config.fit_window_to_usable_bounds = args["window-fit-display"];
      config.high_dpi_allowed = args["high-dpi"];
      config.use_10_bpc = args["10-bpc"];
      config.fast_input_alignment = args["fast-alignment"];
      config.bilinear_texture_filtering = args["bilinear-texture"];
      config.disable_auto_filters = args["disable-auto-filters"];
      config.start_in_subtraction_mode = args["subtraction-mode"];

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
          config.display_mode = Display::Mode::Split;
        } else if (display_mode_arg == "vstack") {
          config.display_mode = Display::Mode::VStack;
        } else if (display_mode_arg == "hstack") {
          config.display_mode = Display::Mode::HStack;
        } else {
          throw std::logic_error{"Cannot parse display mode argument (valid options: split, vstack, hstack)"};
        }
      }
      if (args["color-space"]) {
        auto color_space_spec = static_cast<const std::string&>(args["color-space"]);
        auto left_color_space = get_nth_token_or_empty(color_space_spec, ':', 0);

        config.left.color_space = left_color_space;
        right_template.color_space = (color_space_spec == left_color_space) ? color_space_spec : get_nth_token_or_empty(color_space_spec, ':', 1);
      }
      if (args["color-range"]) {
        auto color_range_spec = static_cast<const std::string&>(args["color-range"]);
        auto left_color_range = get_nth_token_or_empty(color_range_spec, ':', 0);

        config.left.color_range = left_color_range;
        right_template.color_range = (color_range_spec == left_color_range) ? color_range_spec : get_nth_token_or_empty(color_range_spec, ':', 1);
      }
      if (args["color-primaries"]) {
        auto color_primaries_spec = static_cast<const std::string&>(args["color-primaries"]);
        auto left_primaries = get_nth_token_or_empty(color_primaries_spec, ':', 0);

        config.left.color_primaries = left_primaries;
        right_template.color_primaries = (color_primaries_spec == left_primaries) ? color_primaries_spec : get_nth_token_or_empty(color_primaries_spec, ':', 1);
      }
      if (args["color-trc"]) {
        auto color_trc_spec = static_cast<const std::string&>(args["color-trc"]);
        auto left_trc = get_nth_token_or_empty(color_trc_spec, ':', 0);

        config.left.color_trc = left_trc;
        right_template.color_trc = (color_trc_spec == left_trc) ? color_trc_spec : get_nth_token_or_empty(color_trc_spec, ':', 1);
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
          config.auto_loop_mode = Display::Loop::Off;
        } else if (auto_loop_mode_arg == "on") {
          config.auto_loop_mode = Display::Loop::ForwardOnly;
        } else if (auto_loop_mode_arg == "pp") {
          config.auto_loop_mode = Display::Loop::PingPong;
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
        auto tone_mapping_mode_spec = static_cast<const std::string&>(args["tone-map-mode"]);
        auto left_tone_mapping_mode = get_nth_token_or_empty(tone_mapping_mode_spec, ':', 0);

        config.left.tone_mapping_mode = parse_tone_mapping_mode(left_tone_mapping_mode);
        right_template.tone_mapping_mode = (tone_mapping_mode_spec == left_tone_mapping_mode) ? config.left.tone_mapping_mode : parse_tone_mapping_mode(get_nth_token_or_empty(tone_mapping_mode_spec, ':', 1));
      }

      // scopes
      config.scopes.histogram = args["histogram-window"];
      config.scopes.vectorscope = args["vectorscope-window"];
      config.scopes.waveform = args["waveform-window"];
      if (args["histogram-options"]) {
        config.scopes.histogram_options = static_cast<const std::string&>(args["histogram-options"]);
      }
      if (args["vectorscope-options"]) {
        config.scopes.vectorscope_options = static_cast<const std::string&>(args["vectorscope-options"]);
      }
      if (args["waveform-options"]) {
        config.scopes.waveform_options = static_cast<const std::string&>(args["waveform-options"]);
      }
      if (args["scope-size"]) {
        const std::string scope_size_arg = args["scope-size"];
        const std::regex scope_size_re("^(\\d+)x(\\d+)$");
        std::smatch sm;
        if (!std::regex_match(scope_size_arg, sm, scope_size_re)) {
          throw std::logic_error{"Cannot parse --scope-size argument (required format: [width]x[height], e.g. 1024x256)"};
        }
        config.scopes.width = std::stoi(sm[1].str());
        config.scopes.height = std::stoi(sm[2].str());
      }
      if (args["scope-notop"]) {
        config.scopes.always_on_top = false;
      }

      // video filters
      if (args["filters"]) {
        config.left.video_filters = static_cast<const std::string&>(args["filters"]);
        right_template.video_filters = static_cast<const std::string&>(args["filters"]);
      }
      if (args["left-filters"]) {
        config.left.video_filters = safe_replace_placeholder(static_cast<const std::string&>(args["left-filters"]), config.left.video_filters, "filter specification");
      }
      if (args["right-filters"]) {
        right_template.video_filters = safe_replace_placeholder(static_cast<const std::string&>(args["right-filters"]), right_template.video_filters, "filter specification");
      }
      resolve_mutual_placeholders(config.left.video_filters, right_template.video_filters, "filter specification");

      // demuxer
      config.left.demuxer_options = create_default_demuxer_options();
      right_template.demuxer_options = create_default_demuxer_options();

      if (args["demuxer"]) {
        config.left.demuxer = static_cast<const std::string&>(args["demuxer"]);
        right_template.demuxer = static_cast<const std::string&>(args["demuxer"]);
      }
      if (args["left-demuxer"]) {
        config.left.demuxer = safe_replace_placeholder(static_cast<const std::string&>(args["left-demuxer"]), config.left.demuxer, "demuxer");
      }
      if (args["right-demuxer"]) {
        right_template.demuxer = safe_replace_placeholder(static_cast<const std::string&>(args["right-demuxer"]), right_template.demuxer, "demuxer");
      }
      resolve_mutual_placeholders(config.left.demuxer, right_template.demuxer, "demuxer");

      config.left.demuxer = parse_ffmpeg_param_spec(config.left.demuxer, "", config.left.demuxer_options, "demuxer", 1, false, false);
      right_template.demuxer = parse_ffmpeg_param_spec(right_template.demuxer, "", right_template.demuxer_options, "demuxer", 1, false, false);

      // decder
      if (args["decoder"]) {
        config.left.decoder = static_cast<const std::string&>(args["decoder"]);
        right_template.decoder = static_cast<const std::string&>(args["decoder"]);
      }
      if (args["left-decoder"]) {
        config.left.decoder = safe_replace_placeholder(static_cast<const std::string&>(args["left-decoder"]), config.left.decoder, "decoder");
      }
      if (args["right-decoder"]) {
        right_template.decoder = safe_replace_placeholder(static_cast<const std::string&>(args["right-decoder"]), right_template.decoder, "decoder");
      }
      resolve_mutual_placeholders(config.left.decoder, right_template.decoder, "decoder");

      config.left.decoder = parse_ffmpeg_param_spec(config.left.decoder, "", config.left.decoder_options, "decoder", 1, false, false);
      right_template.decoder = parse_ffmpeg_param_spec(right_template.decoder, "", right_template.decoder_options, "decoder", 1, false, false);

      // HW acceleration
      if (args["hwaccel"]) {
        config.left.hw_accel_spec = static_cast<const std::string&>(args["hwaccel"]);
        right_template.hw_accel_spec = static_cast<const std::string&>(args["hwaccel"]);
      }
      if (args["left-hwaccel"]) {
        config.left.hw_accel_spec = safe_replace_placeholder(static_cast<const std::string&>(args["left-hwaccel"]), config.left.hw_accel_spec, "hardware acceleration");
      }
      if (args["right-hwaccel"]) {
        right_template.hw_accel_spec = safe_replace_placeholder(static_cast<const std::string&>(args["right-hwaccel"]), right_template.hw_accel_spec, "hardware acceleration");
      }
      resolve_mutual_placeholders(config.left.hw_accel_spec, right_template.hw_accel_spec, "hardware acceleration");

      config.left.hw_accel_spec = parse_ffmpeg_param_spec(config.left.hw_accel_spec, "", config.left.hw_accel_options, "hardware acceleration", 2, false, true);
      right_template.hw_accel_spec = parse_ffmpeg_param_spec(right_template.hw_accel_spec, "", right_template.hw_accel_options, "hardware acceleration", 2, false, true);

      if (args["left-peak-nits"] || args["right-peak-nits"]) {
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
          config.left.peak_luminance_nits = parse_peak_nits(left_peak_nits);
        }
        if (!right_peak_nits.empty()) {
          right_template.peak_luminance_nits = parse_peak_nits(right_peak_nits);
        }
      }
      if (args["boost-tone"]) {
        auto boost_tone_spec = static_cast<const std::string&>(args["boost-tone"]);
        auto left_boost_tone = get_nth_token_or_empty(boost_tone_spec, ':', 0);

        config.left.boost_tone = parse_boost_tone(left_boost_tone);
        right_template.boost_tone = (boost_tone_spec == left_boost_tone) ? config.left.boost_tone : parse_boost_tone(get_nth_token_or_empty(boost_tone_spec, ':', 1));
      }

      config.left.file_name = args.pos[0];

      // Parse multiple right videos
      // right_template already has all the parsed options
      right_template.file_name = args.pos[1];

      // Resolve placeholders for first right video
      resolve_mutual_placeholders(config.left.file_name, right_template.file_name, "video file", true);

      // Create right videos from all remaining file arguments
      for (size_t i = 1; i < args.pos.size(); ++i) {
        RightVideoSpec spec = parse_right_video_spec(args.pos[i]);

        InputVideo right_video = right_template;
        right_video.file_name = spec.file_name;
        right_video.side = Side::Right(static_cast<size_t>(i - 1));
        right_video.side_description = i == 1 ? "Right" : "Right" + std::to_string(i);
        right_video.demuxer_options = nullptr;
        av_dict_copy(&right_video.demuxer_options, right_template.demuxer_options, 0);
        right_video.decoder_options = nullptr;
        av_dict_copy(&right_video.decoder_options, right_template.decoder_options, 0);
        right_video.hw_accel_options = nullptr;
        av_dict_copy(&right_video.hw_accel_options, right_template.hw_accel_options, 0);

        apply_right_video_spec(right_video, spec, right_template);

        // Resolve placeholders for this right video
        std::string left_file = config.left.file_name;
        resolve_mutual_placeholders(left_file, right_video.file_name, "video file", true);

        config.right_videos.push_back(right_video);
      }

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
