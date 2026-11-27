#pragma once

#include <string>
#include "config.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

namespace hdr_metadata {

struct EffectiveHdr10Tags {
  AVColorPrimaries primaries{AVCOL_PRI_UNSPECIFIED};
  AVColorTransferCharacteristic trc{AVCOL_TRC_UNSPECIFIED};
  AVColorSpace matrix{AVCOL_SPC_UNSPECIFIED};
  AVColorRange range{AVCOL_RANGE_UNSPECIFIED};
  AVPixelFormat pixel_format{AV_PIX_FMT_NONE};
};

void validate_hdr10_cli_color_overrides(const InputVideo& input);
bool is_native_hdr_transfer(AVColorTransferCharacteristic trc);
void validate_effective_native_hdr_tags(const std::string& side, const EffectiveHdr10Tags& tags);
void validate_hdr_filtered_frame(const std::string& side, const AVFrame& frame);
void validate_hdr_converted_frame(const std::string& side, const AVFrame& frame);

}  // namespace hdr_metadata