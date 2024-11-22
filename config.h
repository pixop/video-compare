#pragma once
#include <string>
#include "display.h"
extern "C" {
#include <libavcodec/avcodec.h>
}

enum ToneMapping { off, fullrange, relative };

struct InputVideo {
  std::string file_name;

  std::string video_filters;
  std::string demuxer;
  std::string decoder;
  std::string hw_accel_spec;

  AVDictionary* demuxer_options{nullptr};   // mutated by Demuxer
  AVDictionary* decoder_options{nullptr};   // mutated by VideoDecoder
  AVDictionary* hw_accel_options{nullptr};  // mutated by VideoDecoder

  unsigned peak_luminance_nits{100};  // [cd / m^2]
};

struct VideoCompareConfig {
  bool verbose{false};
  bool fit_window_to_usable_bounds{false};
  bool high_dpi_allowed{false};
  bool use_10_bpc{false};
  bool high_quality_input_alignment{false};
  bool disable_auto_filters{false};

  int display_number{0};
  std::tuple<int, int> window_size{-1, -1};

  Display::Mode display_mode{Display::Mode::split};
  Display::Loop auto_loop_mode{Display::Loop::off};

  ToneMapping tone_mapping_mode{ToneMapping::off};
  float boost_tone{1};

  size_t frame_buffer_size{50};

  double time_shift_ms{0};

  float wheel_sensitivity{1};

  InputVideo left;
  InputVideo right;
};