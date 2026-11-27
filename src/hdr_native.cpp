#include "hdr_native.h"
#include <cctype>
#include <stdexcept>
#include <string>
#include "hdr_metadata.h"
extern "C" {
#include <libavutil/pixdesc.h>
}
#include "sdl_display.h"
#include "string_utils.h"
#include "tone_mapping.h"

namespace hdr_native {
namespace {

bool renderer_is_supported_native_hdr_backend(const std::string& renderer_name) {
  std::string lower = renderer_name;
  for (char& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return lower.find("direct3d11") != std::string::npos || lower.find("metal") != std::string::npos;
}

}  // namespace

void validate_native_hdr_cli_options(const VideoCompareConfig& config) {
  if (!config.native_hdr) {
    return;
  }
  if (!config.use_10_bpc) {
    throw std::runtime_error{"--hdr requires --10-bpc (16-bit RGB processing path)"};
  }
  if (config.start_in_subtraction_mode) {
    throw std::runtime_error{"--hdr does not support starting in subtraction mode"};
  }
  if (config.scopes.histogram || config.scopes.vectorscope || config.scopes.waveform) {
    throw std::runtime_error{"--hdr does not support scope windows at launch"};
  }

  const auto reject_tone_map = [&](const InputVideo& video) {
    if (video.tone_mapping_mode != ToneMapping::Off) {
      throw std::runtime_error{video.side_description + ": --hdr requires -T off"};
    }
    if (video.boost_tone != 1.0F) {
      throw std::runtime_error{video.side_description + ": --boost-tone is not supported with --hdr"};
    }
    hdr_metadata::validate_hdr10_cli_color_overrides(video);
  };

  reject_tone_map(config.left);
  for (const auto& right : config.right_videos) {
    reject_tone_map(right);
  }

  if (1 + config.right_videos.size() < 2) {
    throw std::runtime_error{"--hdr requires at least two HDR10 inputs"};
  }
}

hdr_metadata::EffectiveHdr10Tags effective_hdr10_tags(const VideoDecoder& decoder, const InputVideo& input) {
  hdr_metadata::EffectiveHdr10Tags tags;
  if (!input.color_primaries.empty()) {
    const int value = av_color_primaries_from_name(input.color_primaries.c_str());
    if (value == -1) {
      throw std::runtime_error{"Unknown color primaries '" + input.color_primaries + "'"};
    }
    tags.primaries = static_cast<AVColorPrimaries>(value);
  } else {
    tags.primaries = decoder.color_primaries();
  }
  if (!input.color_trc.empty()) {
    hdr_metadata::validate_hdr10_cli_color_overrides(input);
    const int value = av_color_transfer_from_name(input.color_trc.c_str());
    tags.trc = static_cast<AVColorTransferCharacteristic>(value);
  } else {
    tags.trc = decoder.color_trc();
  }
  if (!input.color_space.empty()) {
    const int value = av_color_space_from_name(input.color_space.c_str());
    if (value == -1) {
      throw std::runtime_error{"Unknown color space '" + input.color_space + "'"};
    }
    tags.matrix = static_cast<AVColorSpace>(value);
  } else {
    tags.matrix = decoder.color_space();
  }
  if (!input.color_range.empty()) {
    if (input.color_range == "tv" || input.color_range == "mpeg" || input.color_range == "limited") {
      tags.range = AVCOL_RANGE_MPEG;
    } else if (input.color_range == "pc" || input.color_range == "jpeg" || input.color_range == "full") {
      tags.range = AVCOL_RANGE_JPEG;
    } else {
      throw std::runtime_error{"Unknown color range '" + input.color_range + "'"};
    }
  } else {
    tags.range = decoder.color_range();
  }
  tags.pixel_format = decoder.pixel_format();
  return tags;
}

void qualify_hdr10_inputs(const VideoCompareConfig& config, const std::map<Side, std::unique_ptr<VideoDecoder>>& decoders) {
  if (!config.native_hdr) {
    return;
  }

  AVColorTransferCharacteristic session_trc = AVCOL_TRC_UNSPECIFIED;

  const auto qualify_one = [&](const Side& side, const InputVideo& input) {
    const auto it = decoders.find(side);
    if (it == decoders.end() || it->second == nullptr) {
      throw std::runtime_error{input.side_description + ": decoder unavailable during HDR qualification"};
    }
    const hdr_metadata::EffectiveHdr10Tags tags = effective_hdr10_tags(*it->second, input);
    hdr_metadata::validate_effective_native_hdr_tags(input.side_description, tags);
    if (session_trc == AVCOL_TRC_UNSPECIFIED) {
      session_trc = tags.trc;
    } else if (tags.trc != session_trc) {
      throw std::runtime_error{"--hdr requires the same HDR transfer on all inputs (do not mix PQ and HLG)"};
    }
  };

  qualify_one(LEFT, config.left);
  for (size_t i = 0; i < config.right_videos.size(); ++i) {
    qualify_one(Side::Right(i), config.right_videos[i]);
  }
}

NativeHdrStatus probe_native_hdr_after_renderer(SDL_Renderer* renderer, SDL_Window* window) {
  NativeHdrStatus status;
  if (renderer == nullptr) {
    return status;
  }

  const SDL_PropertiesID renderer_props = SDL_GetRendererProperties(renderer);
  status.renderer_name = SDL_GetStringProperty(renderer_props, SDL_PROP_RENDERER_NAME_STRING, "unknown");
  status.output_hdr_capable = SDL_GetNumberProperty(renderer_props, SDL_PROP_RENDERER_OUTPUT_COLORSPACE_NUMBER, SDL_COLORSPACE_SRGB) == SDL_COLORSPACE_SRGB_LINEAR;
  status.sdl_sdr_white_level = SDL_GetFloatProperty(renderer_props, SDL_PROP_RENDERER_SDR_WHITE_POINT_FLOAT, 1.0F);
  status.hdr_headroom = SDL_GetFloatProperty(renderer_props, SDL_PROP_RENDERER_HDR_HEADROOM_FLOAT, 1.0F);

  bool renderer_hdr = SDL_GetBooleanProperty(renderer_props, SDL_PROP_RENDERER_HDR_ENABLED_BOOLEAN, false);
  bool window_hdr = false;
  bool display_hdr = false;

  if (window != nullptr) {
    const SDL_PropertiesID window_props = SDL_GetWindowProperties(window);
    window_hdr = SDL_GetBooleanProperty(window_props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
    const SDL_DisplayID display_id = SDL_GetDisplayForWindow(window);
    if (display_id != 0) {
      const SDL_PropertiesID display_props = SDL_GetDisplayProperties(display_id);
      display_hdr = SDL_GetBooleanProperty(display_props, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
    }
  }

  status.display_hdr_enabled = renderer_hdr && window_hdr && display_hdr;
  return status;
}

void require_viable_native_hdr_output(const NativeHdrStatus& status) {
  if (!status.output_hdr_capable) {
    throw std::runtime_error{string_sprintf(
        "Native HDR (--hdr) requires SDL_COLORSPACE_SRGB_LINEAR output; backend '%s' did not select linear HDR output.",
        status.renderer_name.c_str())};
  }
  if (!renderer_is_supported_native_hdr_backend(status.renderer_name)) {
    throw std::runtime_error{string_sprintf(
        "Native HDR (--hdr) requires a D3D11 or Metal renderer; got '%s'.",
        status.renderer_name.c_str())};
  }
  if (!status.display_hdr_enabled) {
    throw std::runtime_error{
        "Native HDR (--hdr) requires HDR/EDR on the window's current display (SDL renderer, window, and display HDR must all be active)."};
  }
  if (status.sdl_sdr_white_level <= 0.0F) {
    throw std::runtime_error{"Native HDR: invalid SDL SDR white level"};
  }
}

}  // namespace hdr_native