#pragma once

#include <SDL3/SDL.h>
#include <map>
#include <memory>
#include <string>
#include "config.h"
#include "core_types.h"
#include "hdr_color.h"
#include "hdr_metadata.h"
#include "demuxer.h"
#include "video_decoder.h"

namespace hdr_native {

struct NativeHdrStatus {
  bool output_hdr_capable{false};
  bool display_hdr_enabled{false};
  float sdl_sdr_white_level{1.0F};
  float hdr_headroom{1.0F};
  std::string renderer_name;
};

void validate_native_hdr_cli_options(const VideoCompareConfig& config);
hdr_metadata::EffectiveHdr10Tags effective_hdr10_tags(const VideoDecoder& decoder, const InputVideo& input);
void qualify_hdr10_inputs(const VideoCompareConfig& config, const std::map<Side, std::unique_ptr<VideoDecoder>>& decoders);
NativeHdrStatus probe_native_hdr_after_renderer(SDL_Renderer* renderer, SDL_Window* window);
void require_viable_native_hdr_output(const NativeHdrStatus& status);

}  // namespace hdr_native