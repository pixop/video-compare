#pragma once

#include <string>
#include <vector>
#include "core_types.h"

enum class ToneMapAlgorithm { Clip, Mobius, Hable, Reinhard };

constexpr unsigned SDR_REFERENCE_WHITE_NITS = 100;
constexpr char TONEMAP_NORM_PEAK_TOKEN[] = "@@TONEMAP_NORM_PEAK@@";
constexpr char TONE_MAP_PEAK_NITS_TOKEN[] = "%d";

ToneMapAlgorithm parse_tone_map_algorithm(const std::string& algorithm);
const char* tone_map_algorithm_name(const ToneMapAlgorithm algorithm);

// FFmpeg tonemap peak is linear signal peak relative to SDR reference white (1.0 ~= 100 nits).
double normalized_tonemap_peak(const unsigned peak_luminance_nits, const float boost_tone);
std::string format_normalized_tonemap_peak(const unsigned peak_luminance_nits, const float boost_tone);

// Relative (-T rel) tone mapping only supports clip gain via tonemap=clip:param=.
void validate_tone_mapping_for_side(const std::string& side_description, const ToneMapping tone_mapping_mode, const ToneMapAlgorithm tone_map_algorithm);

struct ToneMapFilterPlan {
  std::vector<std::string> post_filters;
  bool inject_peak_nits{false};
  bool inject_auto_tonemap_peak{false};
};

// Builds HDR tone-map post-filters (after setparams). Empty if filters are unavailable.
ToneMapFilterPlan build_hdr_tone_map_filters(const ToneMapping tone_mapping_mode,
                                             const ToneMapAlgorithm tone_map_algorithm,
                                             const float boost_tone,
                                             const unsigned peak_luminance_nits,
                                             const unsigned other_peak_luminance_nits,
                                             const bool have_zscale,
                                             const bool have_tonemap);

std::string resolve_tone_map_filter_description(const std::string& description, const unsigned peak_luminance_nits, const float boost_tone, const bool inject_peak_nits, const bool inject_auto_tonemap_peak);