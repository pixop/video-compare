#include "hdr_metadata.h"
#include <stdexcept>
#include <string>
#include "string_utils.h"
extern "C" {
#include <libavutil/pixdesc.h>
}

namespace hdr_metadata {
namespace {

AVColorPrimaries parse_primaries_name(const std::string& name) {
  if (name.empty()) {
    return AVCOL_PRI_UNSPECIFIED;
  }
  const int value = av_color_primaries_from_name(name.c_str());
  if (value == -1) {
    throw std::runtime_error{"Unknown color primaries '" + name + "'"};
  }
  return static_cast<AVColorPrimaries>(value);
}

AVColorTransferCharacteristic parse_trc_name(const std::string& name) {
  if (name.empty()) {
    return AVCOL_TRC_UNSPECIFIED;
  }
  if (name == "bt2020-10" || name == "bt2020-12") {
    throw std::runtime_error{"Transfer '" + name + "' is not PQ; use smpte2084 for HDR10"};
  }
  const int value = av_color_transfer_from_name(name.c_str());
  if (value == -1) {
    throw std::runtime_error{"Unknown transfer characteristics '" + name + "'"};
  }
  return static_cast<AVColorTransferCharacteristic>(value);
}

AVColorSpace parse_matrix_name(const std::string& name) {
  if (name.empty()) {
    return AVCOL_SPC_UNSPECIFIED;
  }
  if (name == "bt2020c" || name == "bt2020-cl" || name == "bt2020_cl") {
    throw std::runtime_error{"Color matrix '" + name + "' is BT.2020 CL, not supported for --hdr (use bt2020nc)"};
  }
  const int value = av_color_space_from_name(name.c_str());
  if (value == -1) {
    throw std::runtime_error{"Unknown color space '" + name + "'"};
  }
  return static_cast<AVColorSpace>(value);
}

AVColorRange parse_range_name(const std::string& name) {
  if (name.empty()) {
    return AVCOL_RANGE_UNSPECIFIED;
  }
  if (name == "tv" || name == "mpeg" || name == "limited") {
    return AVCOL_RANGE_MPEG;
  }
  if (name == "pc" || name == "jpeg" || name == "full") {
    return AVCOL_RANGE_JPEG;
  }
  throw std::runtime_error{"Unknown color range '" + name + "'"};
}

}  // namespace

void validate_hdr10_cli_color_overrides(const InputVideo& input) {
  if (!input.color_primaries.empty()) {
    parse_primaries_name(input.color_primaries);
  }
  if (!input.color_trc.empty()) {
    parse_trc_name(input.color_trc);
  }
  if (!input.color_space.empty()) {
    parse_matrix_name(input.color_space);
  }
  if (!input.color_range.empty()) {
    parse_range_name(input.color_range);
  }
}

bool is_native_hdr_transfer(const AVColorTransferCharacteristic trc) {
  return trc == AVCOL_TRC_SMPTE2084 || trc == AVCOL_TRC_ARIB_STD_B67;
}

void validate_effective_native_hdr_tags(const std::string& side, const EffectiveHdr10Tags& tags) {
  if (!is_native_hdr_transfer(tags.trc)) {
    throw std::runtime_error{side + ": --hdr requires smpte2084 (PQ) or arib-std-b67 (HLG)"};
  }
  if (tags.primaries != AVCOL_PRI_BT2020) {
    throw std::runtime_error{side + ": --hdr requires BT.2020 primaries; effective primaries are not BT.2020"};
  }
  if (tags.matrix != AVCOL_SPC_BT2020_NCL && tags.matrix != AVCOL_SPC_UNSPECIFIED) {
    if (tags.matrix == AVCOL_SPC_BT2020_CL) {
      throw std::runtime_error{side + ": BT.2020 CL matrix is not supported for --hdr (use bt2020nc)"};
    }
    throw std::runtime_error{side + ": --hdr requires bt2020nc color matrix (non-constant luminance)"};
  }
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(tags.pixel_format);
  if (desc == nullptr) {
    throw std::runtime_error{side + ": unknown decoder pixel format"};
  }
  const bool yuv = (desc->flags & AV_PIX_FMT_FLAG_RGB) == 0;
  const bool rgb = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
  if (!yuv && !rgb) {
    throw std::runtime_error{side + ": --hdr expects YUV or RGB video; unsupported pixel layout"};
  }
}

void validate_hdr_filtered_frame(const std::string& side, const AVFrame& frame) {
  if (frame.format != AV_PIX_FMT_RGB48LE) {
    throw std::runtime_error{side + ": native HDR expects RGB48LE after filtering; got unexpected pixel format"};
  }
  if (!is_native_hdr_transfer(frame.color_trc) && frame.color_trc != AVCOL_TRC_UNSPECIFIED) {
    throw std::runtime_error{side + ": filtered frame has unsupported HDR transfer metadata"};
  }
  if (frame.colorspace != AVCOL_SPC_BT2020_NCL && frame.colorspace != AVCOL_SPC_BT2020_CL && frame.colorspace != AVCOL_SPC_UNSPECIFIED) {
    throw std::runtime_error{side + ": filtered frame has unexpected colorspace after filters"};
  }
  if (frame.colorspace == AVCOL_SPC_BT2020_CL) {
    throw std::runtime_error{side + ": filtered frame is BT.2020 CL; not supported on --hdr path"};
  }
}

void validate_hdr_converted_frame(const std::string& side, const AVFrame& frame) {
  if (frame.format != AV_PIX_FMT_RGB48LE) {
    throw std::runtime_error{side + ": native HDR conversion must output RGB48LE"};
  }
  if (frame.color_range != AVCOL_RANGE_JPEG) {
    throw std::runtime_error{side + ": native HDR conversion must output full-range (pc) RGB48"};
  }
  if (frame.colorspace == AVCOL_SPC_BT2020_CL) {
    throw std::runtime_error{side + ": native HDR conversion produced BT.2020 CL"};
  }
}

}  // namespace hdr_metadata