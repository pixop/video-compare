#pragma once

#include <string>

// Color representation analyzed by a scope. This is not the source pixel format.
enum class ScopeColorMode { Rgb, Yuv };

struct ScopeColorModes {
  ScopeColorMode histogram{ScopeColorMode::Rgb};
  ScopeColorMode vectorscope{ScopeColorMode::Yuv};
  ScopeColorMode waveform{ScopeColorMode::Yuv};
};

// [histogram][:vectorscope][:waveform]
// A value with no colon applies to all three scopes. Colon-separated fields are
// positional: an empty field, or a missing trailing field, keeps that scope's default.
// Throws std::logic_error for an unrecognized value or more than three fields.
ScopeColorModes parse_scope_color_modes(const std::string& spec);

// Planar pixel format accepted by the histogram, vectorscope, and waveform filters.
const char* scope_analysis_pix_fmt(ScopeColorMode mode, bool use_10_bpc);

// FFmpeg filter invocation, including options. An RGB waveform shows all three
// channels unless the supplied options already set components or c.
std::string scope_filter_spec(const char* filter_name, ScopeColorMode mode, const std::string& user_options);
