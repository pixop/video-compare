#include "scope_color.h"
#include <stdexcept>
#include <vector>

namespace {

constexpr const char* kValidScopeColorModes = "rgb, yuv";

std::vector<std::string> split_scope_color_fields(const std::string& spec) {
  // Keep a trailing empty field. string_split() drops it, which would hide an extra position.
  std::vector<std::string> fields;
  std::string field;
  for (const char ch : spec) {
    if (ch == ':') {
      fields.push_back(field);
      field.clear();
    } else {
      field.push_back(ch);
    }
  }
  fields.push_back(field);
  return fields;
}

ScopeColorMode parse_scope_color_mode(const std::string& value) {
  if (value == "rgb") {
    return ScopeColorMode::Rgb;
  }
  if (value == "yuv") {
    return ScopeColorMode::Yuv;
  }
  throw std::logic_error{"Cannot parse scope color mode '" + value + "' (valid options: " + kValidScopeColorModes + ")"};
}

bool scope_filter_option_set(const std::string& options, const std::string& key) {
  std::string field;
  const auto field_matches = [](const std::string& candidate, const std::string& option_key) {
    const size_t eq = candidate.find('=');
    const std::string name = eq == std::string::npos ? candidate : candidate.substr(0, eq);
    return name == option_key;
  };

  for (const char ch : options) {
    if (ch == ':') {
      if (field_matches(field, key)) {
        return true;
      }
      field.clear();
    } else {
      field.push_back(ch);
    }
  }
  return field_matches(field, key);
}

std::string effective_scope_filter_options(const char* filter_name, const ScopeColorMode mode, const std::string& user_options) {
  // Waveform's default components=1 is only red on planar RGB.
  if (filter_name != nullptr && std::string(filter_name) == "waveform" && mode == ScopeColorMode::Rgb && !scope_filter_option_set(user_options, "components") && !scope_filter_option_set(user_options, "c")) {
    if (user_options.empty()) {
      return "components=7";
    }
    return user_options + ":components=7";
  }
  return user_options;
}

}  // namespace

ScopeColorModes parse_scope_color_modes(const std::string& spec) {
  const std::vector<std::string> fields = split_scope_color_fields(spec);
  if (fields.size() > 3) {
    throw std::logic_error{"Cannot parse scope color mode (expected at most 3 colon-separated values)"};
  }

  ScopeColorModes modes;
  if (fields.size() == 1) {
    const ScopeColorMode mode = parse_scope_color_mode(fields[0]);
    modes.histogram = mode;
    modes.vectorscope = mode;
    modes.waveform = mode;
    return modes;
  }

  if (!fields[0].empty()) {
    modes.histogram = parse_scope_color_mode(fields[0]);
  }
  if (!fields[1].empty()) {
    modes.vectorscope = parse_scope_color_mode(fields[1]);
  }
  if (fields.size() == 3 && !fields[2].empty()) {
    modes.waveform = parse_scope_color_mode(fields[2]);
  }
  return modes;
}

const char* scope_analysis_pix_fmt(const ScopeColorMode mode, const bool use_10_bpc) {
  if (mode == ScopeColorMode::Rgb) {
    return use_10_bpc ? "gbrp10" : "gbrp";
  }
  return use_10_bpc ? "yuv444p10le" : "yuv444p";
}

std::string scope_filter_spec(const char* filter_name, const ScopeColorMode mode, const std::string& user_options) {
  const std::string name = filter_name == nullptr ? std::string() : std::string(filter_name);
  const std::string options = effective_scope_filter_options(filter_name, mode, user_options);
  if (options.empty()) {
    return name;
  }
  return name + "=" + options;
}
