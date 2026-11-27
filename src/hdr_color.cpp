#include "hdr_color.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace hdr_color {
namespace {

constexpr float kPqC1 = 0.8359375F;
constexpr float kPqC2 = 18.8515625F;
constexpr float kPqC3 = 18.6875F;
constexpr float kPqOm1 = 1.0F / 0.1593017578125F;
constexpr float kPqOm2 = 1.0F / 78.84375F;
constexpr float kPqM1 = 0.1593017578125F;
constexpr float kPqM2 = 78.84375F;

std::array<float, 65536> g_pq_rgb48_full_to_nits{};
std::once_flag g_pq_lut_once;

void build_pq_rgb48_lut() {
  for (uint32_t i = 0; i < 65536U; ++i) {
    const float pq = static_cast<float>(i) / 65535.0F;
    const float ypow = std::pow(pq, kPqOm2);
    const float num = std::max(ypow - kPqC1, 0.0F);
    const float den = kPqC2 - kPqC3 * ypow;
    g_pq_rgb48_full_to_nits[i] = (den > 0.0F) ? (kPqSignalMaxNits * std::pow(num / den, kPqOm1)) : 0.0F;
  }
}

}  // namespace

float platform_scrgb_content_white_nits() {
#if defined(_WIN32)
  return kWindowsScrgbContentWhiteNits;
#elif defined(__APPLE__)
  return kMacOsNominalEdrWhiteNits;
#else
  return kWindowsScrgbContentWhiteNits;
#endif
}

void ensure_pq_rgb48_lut_initialized() {
  std::call_once(g_pq_lut_once, build_pq_rgb48_lut);
}

float pq_signal_to_linear_nits(float pq_signal) {
  pq_signal = std::max(0.0F, pq_signal);
  const float ypow = std::pow(pq_signal, kPqOm2);
  const float num = std::max(ypow - kPqC1, 0.0F);
  const float den = kPqC2 - kPqC3 * ypow;
  if (den <= 0.0F) {
    return 0.0F;
  }
  return kPqSignalMaxNits * std::pow(num / den, kPqOm1);
}

float linear_nits_to_pq_signal(float nits) {
  nits = std::max(0.0F, nits);
  const float yn = std::min(1.0F, std::max(0.0F, nits / kPqSignalMaxNits));
  const float y = std::pow(yn, kPqM1);
  const float num = kPqC1 + kPqC2 * y;
  const float den = 1.0F + kPqC3 * y;
  return std::pow(num / den, kPqM2);
}

float pq_rgb48_full_range_sample_to_nits(uint16_t sample) {
  ensure_pq_rgb48_lut_initialized();
  return g_pq_rgb48_full_to_nits[sample];
}

std::array<float, 3> mat3_mul(const std::array<float, 9>& m, const std::array<float, 3>& v) {
  return {
      m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
      m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
      m[6] * v[0] + m[7] * v[1] + m[8] * v[2],
  };
}

std::array<float, 3> linear_nits_bt2020_neutral_to_scrgb(const float nits, const float scrgb_content_white_nits) {
  const float denom = std::max(scrgb_content_white_nits, 1.0F);
  const float v = nits / denom;
  return mat3_mul(kBt2020ToBt709Matrix, {v, v, v});
}

float hlg_signal_to_relative_linear(const float signal) {
  const float e = std::max(0.0F, signal);
  if (e <= 0.5F) {
    return e * e / 3.0F;
  }
  constexpr float a = 0.17883277F;
  constexpr float b = 0.28466892F;
  constexpr float c = 0.55991073F;
  return (std::exp((e - c) / a) + b) / 12.0F;
}

float hlg_signal_to_display_nits(const float signal, const float hlg_reference_peak_nits) {
  const float relative = hlg_signal_to_relative_linear(signal);
  return hlg_reference_peak_nits * std::pow(std::max(relative, 0.0F), kHlgSystemGamma);
}

float hlg_rgb48_full_range_sample_to_signal(const uint16_t sample) {
  return static_cast<float>(sample) / 65535.0F;
}

float hlg_rgb48_full_range_sample_to_nits(const uint16_t sample, const float hlg_reference_peak_nits) {
  return hlg_signal_to_display_nits(hlg_rgb48_full_range_sample_to_signal(sample), hlg_reference_peak_nits);
}

float nits_from_rgb48_channel(const uint16_t sample, const AVColorTransferCharacteristic trc, const float hlg_reference_peak_nits) {
  if (trc == AVCOL_TRC_SMPTE2084) {
    return pq_rgb48_full_range_sample_to_nits(sample);
  }
  if (trc == AVCOL_TRC_ARIB_STD_B67) {
    return hlg_rgb48_full_range_sample_to_nits(sample, hlg_reference_peak_nits);
  }
  return 0.0F;
}

std::array<float, 3> hdr_rgb48_full_range_pixel_to_scrgb_linear(const uint16_t r, const uint16_t g, const uint16_t b, const AVColorTransferCharacteristic trc, const float scrgb_content_white_nits,
                                                                const float hlg_reference_peak_nits) {
  const float denom = std::max(scrgb_content_white_nits, 1.0F);
  const float nr = nits_from_rgb48_channel(r, trc, hlg_reference_peak_nits) / denom;
  const float ng = nits_from_rgb48_channel(g, trc, hlg_reference_peak_nits) / denom;
  const float nb = nits_from_rgb48_channel(b, trc, hlg_reference_peak_nits) / denom;
  return mat3_mul(kBt2020ToBt709Matrix, {nr, ng, nb});
}

std::array<float, 3> pq_rgb48_full_range_pixel_to_scrgb_linear(const uint16_t r, const uint16_t g, const uint16_t b, const float scrgb_content_white_nits) {
  return hdr_rgb48_full_range_pixel_to_scrgb_linear(r, g, b, AVCOL_TRC_SMPTE2084, scrgb_content_white_nits, kHlgReferencePeakNits);
}

uint16_t float_to_fp16(float value) {
  if (std::isnan(value)) {
    return 0x7e00U;
  }
  if (std::isinf(value)) {
    return value < 0.0F ? 0xfc00U : 0x7c00U;
  }

  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));

  const uint32_t sign = (bits >> 16U) & 0x8000U;
  int32_t exponent = static_cast<int32_t>((bits >> 23U) & 0xffU) - 127 + 15;
  uint32_t mantissa = bits & 0x7fffffU;

  if (exponent <= 0) {
    if (exponent < -10) {
      return static_cast<uint16_t>(sign);
    }
    mantissa |= 0x800000U;
    const uint32_t shift = static_cast<uint32_t>(14 - exponent);
    mantissa >>= shift;
    return static_cast<uint16_t>(sign | mantissa);
  }
  if (exponent >= 31) {
    return static_cast<uint16_t>(sign | 0x7c00U);
  }
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10U) | (mantissa >> 13U));
}

}  // namespace hdr_color