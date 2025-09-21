#pragma once
#include <string>
#include "core_types.h"
#include "display.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

struct TimeShiftConfig {
  AVRational multiplier{1, 1};
  int64_t offset_ms{0};
};

struct InputVideo {
  Side side;
  std::string side_description;

  std::string file_name;

  std::string video_filters;
  std::string demuxer;
  std::string decoder;
  std::string hw_accel_spec;

  std::string color_space;
  std::string color_range;
  std::string color_primaries;
  std::string color_trc;

  AVDictionary* demuxer_options{nullptr};   // mutated by Demuxer
  AVDictionary* decoder_options{nullptr};   // mutated by VideoDecoder
  AVDictionary* hw_accel_options{nullptr};  // mutated by VideoDecoder

  ToneMapping tone_mapping_mode{ToneMapping::AUTO};
  unsigned peak_luminance_nits{UNSET_PEAK_LUMINANCE};  // [cd / m^2]
  float boost_tone{1};
};

struct VideoCompareConfig {
  bool verbose{false};
  bool fit_window_to_usable_bounds{false};
  bool high_dpi_allowed{false};
  bool use_10_bpc{false};
  bool fast_input_alignment{false};
  bool bilinear_texture_filtering{false};
  bool disable_auto_filters{false};

  int display_number{0};
  std::tuple<int, int> window_size{-1, -1};

  Display::Mode display_mode{Display::Mode::SPLIT};
  Display::Loop auto_loop_mode{Display::Loop::OFF};

  size_t frame_buffer_size{50};

  TimeShiftConfig time_shift;

  float wheel_sensitivity{1};

  InputVideo left{Side::LEFT, "Left"};
  InputVideo right{Side::RIGHT, "Right"};
};