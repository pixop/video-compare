#include "tone_mapping.h"
#include <cmath>
#include <stdexcept>
#include "string_utils.h"

ToneMapAlgorithm parse_tone_map_algorithm(const std::string& algorithm) {
  if (algorithm.empty() || algorithm == "clip") {
    return ToneMapAlgorithm::Clip;
  }
  if (algorithm == "mobius") {
    return ToneMapAlgorithm::Mobius;
  }
  if (algorithm == "hable") {
    return ToneMapAlgorithm::Hable;
  }
  if (algorithm == "reinhard") {
    return ToneMapAlgorithm::Reinhard;
  }
  throw std::logic_error{"Cannot parse tone map algorithm (valid options: clip, mobius, hable, reinhard)"};
}

const char* tone_map_algorithm_name(const ToneMapAlgorithm algorithm) {
  switch (algorithm) {
    case ToneMapAlgorithm::Clip:
      return "clip";
    case ToneMapAlgorithm::Mobius:
      return "mobius";
    case ToneMapAlgorithm::Hable:
      return "hable";
    case ToneMapAlgorithm::Reinhard:
      return "reinhard";
  }
  return "clip";
}

double normalized_tonemap_peak(const unsigned peak_luminance_nits, const float boost_tone) {
  return static_cast<double>(peak_luminance_nits) / static_cast<double>(SDR_REFERENCE_WHITE_NITS) * static_cast<double>(boost_tone);
}

std::string format_normalized_tonemap_peak(const unsigned peak_luminance_nits, const float boost_tone) {
  return string_sprintf("%.6f", normalized_tonemap_peak(peak_luminance_nits, boost_tone));
}

void validate_tone_mapping_for_side(const std::string& side_description, const ToneMapping tone_mapping_mode, const ToneMapAlgorithm tone_map_algorithm) {
  if (tone_mapping_mode == ToneMapping::Relative && tone_map_algorithm != ToneMapAlgorithm::Clip) {
    throw std::logic_error{side_description + ": relative tone mapping (-T rel) requires the clip algorithm (--tone-map-algorithm clip); got " + tone_map_algorithm_name(tone_map_algorithm)};
  }
}

static void replace_first_token(std::string& text, const std::string& token, const std::string& value) {
  const size_t pos = text.find(token);
  if (pos != std::string::npos) {
    text.replace(pos, token.size(), value);
  }
}

std::string resolve_tone_map_filter_description(const std::string& description,
                                                  const unsigned peak_luminance_nits,
                                                  const float boost_tone,
                                                  const bool inject_peak_nits,
                                                  const bool inject_auto_tonemap_peak) {
  std::string result = description;
  if (inject_peak_nits) {
    replace_first_token(result, TONE_MAP_PEAK_NITS_TOKEN, std::to_string(peak_luminance_nits));
  }
  if (inject_auto_tonemap_peak) {
    replace_first_token(result, TONEMAP_NORM_PEAK_TOKEN, format_normalized_tonemap_peak(peak_luminance_nits, boost_tone));
  }
  return result;
}

static void append_linear_boost(std::vector<std::string>& filters, const float boost_tone) {
  if (std::fabs(boost_tone - 1.0F) <= 1e-5F) {
    return;
  }

  const float exposure_ev = std::log2(boost_tone);
  if (exposure_ev < -3.0F || exposure_ev > 3.0F) {
    throw std::logic_error{"-B/--boost-tone is out of range for nonlinear tone mapping with the exposure filter (linear gain must be between 0.125 and 8)"};
  }

  filters.push_back(string_sprintf("exposure=%.6f", exposure_ev));
}

ToneMapFilterPlan build_hdr_tone_map_filters(const ToneMapping tone_mapping_mode,
                                             const ToneMapAlgorithm tone_map_algorithm,
                                             const float boost_tone,
                                             const unsigned peak_luminance_nits,
                                             const unsigned other_peak_luminance_nits,
                                             const bool have_zscale,
                                             const bool have_tonemap) {
  ToneMapFilterPlan plan;
  if (!have_zscale) {
    return plan;
  }

  const std::string display_primaries = "bt709";
  const std::string display_trc = "iec61966-2-1";
  const bool auto_peak = tone_mapping_mode == ToneMapping::Auto;

  float tone_adjustment = (tone_mapping_mode == ToneMapping::Relative && peak_luminance_nits < other_peak_luminance_nits) ? static_cast<float>(peak_luminance_nits) / static_cast<float>(other_peak_luminance_nits) : 1.0F;
  tone_adjustment *= boost_tone;

  if (tone_map_algorithm == ToneMapAlgorithm::Clip) {
    if (std::fabs(tone_adjustment - 1.0F) > 1e-5F) {
      plan.post_filters.push_back("format=gbrpf32");
      if (auto_peak) {
        plan.post_filters.push_back("zscale=t=linear:npl=%d");
        plan.inject_peak_nits = true;
      } else {
        plan.post_filters.push_back(string_sprintf("zscale=t=linear:npl=%d", peak_luminance_nits));
      }
      plan.post_filters.push_back(string_sprintf("tonemap=clip:param=%.5f", tone_adjustment));
      plan.post_filters.push_back(string_sprintf("zscale=p=%s:t=%s", display_primaries.c_str(), display_trc.c_str()));
    } else {
      plan.post_filters.push_back("format=rgb48");
      if (auto_peak) {
        plan.post_filters.push_back(string_sprintf("zscale=p=%s:t=%s:npl=%%d", display_primaries.c_str(), display_trc.c_str()));
        plan.inject_peak_nits = true;
      } else {
        plan.post_filters.push_back(string_sprintf("zscale=p=%s:t=%s:npl=%d", display_primaries.c_str(), display_trc.c_str(), peak_luminance_nits));
      }
    }
    return plan;
  }

  if (!have_tonemap) {
    return plan;
  }

  plan.post_filters.push_back("format=gbrpf32");
  plan.post_filters.push_back(string_sprintf("zscale=t=linear:npl=%d", SDR_REFERENCE_WHITE_NITS));
  append_linear_boost(plan.post_filters, boost_tone);

  const char* algorithm = tone_map_algorithm_name(tone_map_algorithm);
  if (auto_peak) {
    plan.post_filters.push_back(string_sprintf("tonemap=%s:peak=%s", algorithm, TONEMAP_NORM_PEAK_TOKEN));
    plan.inject_auto_tonemap_peak = true;
  } else {
    plan.post_filters.push_back(string_sprintf("tonemap=%s:peak=%s", algorithm, format_normalized_tonemap_peak(peak_luminance_nits, boost_tone).c_str()));
  }
  plan.post_filters.push_back(string_sprintf("zscale=p=%s:t=%s", display_primaries.c_str(), display_trc.c_str()));
  return plan;
}