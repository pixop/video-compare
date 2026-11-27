#pragma once

#include <array>
#include <cstdint>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace hdr_color {

// Absolute scRGB content encoding reference (1.0 scRGB = this many nits at neutral).
// Windows DXGI scRGB uses 80 nits per 1.0. macOS value is a nominal EDR reference only (not calibrated).
constexpr float kWindowsScrgbContentWhiteNits = 80.0F;
constexpr float kMacOsNominalEdrWhiteNits = 100.0F;

constexpr float kPqSignalMaxNits = 10000.0F;

float platform_scrgb_content_white_nits();

// BT.709 <- BT.2020 (column vectors), matching SDL D3D11/Metal pixel shaders (mat2020to709).
constexpr std::array<float, 9> kBt2020ToBt709Matrix = {{
    1.660496F, -0.587656F, -0.072840F,
    -0.124547F, 1.132895F, -0.008348F,
    -0.018154F, -0.100597F, 1.118751F,
}};

float pq_signal_to_linear_nits(float pq_signal);
float linear_nits_to_pq_signal(float nits);
float pq_rgb48_full_range_sample_to_nits(uint16_t sample);

// BT.2100 HLG: inverse OETF + OOTF at `hlg_reference_peak_nits` (default 1000).
constexpr float kHlgReferencePeakNits = 1000.0F;
constexpr float kHlgSystemGamma = 1.2F;

float hlg_signal_to_relative_linear(float signal);
float hlg_signal_to_display_nits(float signal, float hlg_reference_peak_nits = kHlgReferencePeakNits);
float hlg_rgb48_full_range_sample_to_signal(uint16_t sample);
float hlg_rgb48_full_range_sample_to_nits(uint16_t sample, float hlg_reference_peak_nits = kHlgReferencePeakNits);

std::array<float, 3> linear_nits_bt2020_neutral_to_scrgb(float nits, float scrgb_content_white_nits);
std::array<float, 3> pq_rgb48_full_range_pixel_to_scrgb_linear(uint16_t r, uint16_t g, uint16_t b, float scrgb_content_white_nits);
std::array<float, 3> hdr_rgb48_full_range_pixel_to_scrgb_linear(uint16_t r, uint16_t g, uint16_t b, AVColorTransferCharacteristic trc, float scrgb_content_white_nits,
                                                                float hlg_reference_peak_nits = kHlgReferencePeakNits);

float nits_from_rgb48_channel(uint16_t sample, AVColorTransferCharacteristic trc, float hlg_reference_peak_nits = kHlgReferencePeakNits);

std::array<float, 3> mat3_mul(const std::array<float, 9>& m, const std::array<float, 3>& v);

uint16_t float_to_fp16(float value);

void ensure_pq_rgb48_lut_initialized();

}  // namespace hdr_color