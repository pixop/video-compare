#include "hdr_color.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

bool near(float a, float b, float eps) {
  return std::fabs(a - b) <= eps;
}

int fail(const std::string& msg) {
  std::cerr << "FAIL: " << msg << "\n";
  return EXIT_FAILURE;
}

int test_pq_reference_luminance() {
  const float pq_100nits = 0.508078421517399F;
  const float nits = hdr_color::pq_signal_to_linear_nits(pq_100nits);
  if (!near(nits, 100.0F, 0.5F)) {
    return fail("PQ reference luminance");
  }
  return EXIT_SUCCESS;
}

int test_scrgb_neutral_nits(float nits, float reference_nits) {
  const float pq = hdr_color::linear_nits_to_pq_signal(nits);
  const float pq_clamped = std::min(1.0F, std::max(0.0F, pq));
  const uint16_t sample = static_cast<uint16_t>(std::lround(pq_clamped * 65535.0F));
  const auto scrgb = hdr_color::pq_rgb48_full_range_pixel_to_scrgb_linear(sample, sample, sample, reference_nits);
  const float expected = nits / reference_nits;
  if (!near(scrgb[0], expected, 0.03F) || !near(scrgb[1], expected, 0.03F) || !near(scrgb[2], expected, 0.03F)) {
    return fail("scRGB neutral at " + std::to_string(nits) + " nits");
  }
  return EXIT_SUCCESS;
}

int test_scrgb_neutrals_windows() {
  const float ref = hdr_color::kWindowsScrgbContentWhiteNits;
  for (const float nits : {80.0F, 100.0F, 203.0F, 1000.0F}) {
    if (test_scrgb_neutral_nits(nits, ref) != EXIT_SUCCESS) {
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}

int test_sdl_level_not_used_as_nits() {
  const float ref = hdr_color::platform_scrgb_content_white_nits();
  const float pq = hdr_color::linear_nits_to_pq_signal(100.0F);
  const uint16_t sample = static_cast<uint16_t>(std::lround(pq * 65535.0F));
  const auto correct = hdr_color::pq_rgb48_full_range_pixel_to_scrgb_linear(sample, sample, sample, ref);
  const auto wrong = hdr_color::pq_rgb48_full_range_pixel_to_scrgb_linear(sample, sample, sample, 1.0F);
  if (near(correct[0], wrong[0], 0.1F)) {
    return fail("using SDL normalized level (1.0) as nits must not match platform reference");
  }
  return EXIT_SUCCESS;
}

int test_above_one_scrgb() {
  const float ref = hdr_color::platform_scrgb_content_white_nits();
  const uint16_t hot = 52000;
  const auto scrgb = hdr_color::pq_rgb48_full_range_pixel_to_scrgb_linear(hot, hot, hot, ref);
  if (scrgb[0] <= 1.0F) {
    return fail("expected scRGB > 1.0 for bright PQ");
  }
  return EXIT_SUCCESS;
}

int test_wide_gamut_negative_channel() {
  const float ref = hdr_color::platform_scrgb_content_white_nits();
  const uint16_t full = 60000;
  const auto scrgb = hdr_color::pq_rgb48_full_range_pixel_to_scrgb_linear(full, 8192, 8192, ref);
  if (scrgb[1] >= 0.0F || scrgb[2] >= 0.0F) {
    return fail("expected negative scRGB channels outside 709 gamut");
  }
  return EXIT_SUCCESS;
}

int test_lut_matches_direct() {
  hdr_color::ensure_pq_rgb48_lut_initialized();
  for (uint16_t s : {0U, 1000U, 32768U, 60000U, 65535U}) {
    const float direct = hdr_color::pq_signal_to_linear_nits(static_cast<float>(s) / 65535.0F);
    const float lut = hdr_color::pq_rgb48_full_range_sample_to_nits(s);
    if (!near(direct, lut, 0.05F)) {
      return fail("PQ LUT mismatch");
    }
  }
  return EXIT_SUCCESS;
}

int test_sdr_regression_unchanged() {
  if (hdr_color::pq_signal_to_linear_nits(0.0F) != 0.0F) {
    return fail("PQ zero");
  }
  return EXIT_SUCCESS;
}

int test_hlg_ootf_reference_display() {
  const float nits = hdr_color::hlg_signal_to_display_nits(0.75F);
  if (!near(nits, 203.0F, 4.0F)) {
    return fail("HLG 0.75 signal OOTF display nits");
  }
  return EXIT_SUCCESS;
}

int test_hlg_rgb48_scrgb_neutral() {
  const float ref = hdr_color::platform_scrgb_content_white_nits();
  const uint16_t sample = static_cast<uint16_t>(std::lround(0.75F * 65535.0F));
  const auto scrgb = hdr_color::hdr_rgb48_full_range_pixel_to_scrgb_linear(sample, sample, sample, AVCOL_TRC_ARIB_STD_B67, ref);
  const float expected = hdr_color::hlg_signal_to_display_nits(0.75F) / ref;
  if (!near(scrgb[0], expected, 0.06F)) {
    return fail("HLG neutral scRGB");
  }
  return EXIT_SUCCESS;
}

}  // namespace

int main() {
  const struct {
    const char* name;
    int (*fn)();
  } cases[] = {
      {"pq_reference", test_pq_reference_luminance},
      {"scrgb_neutrals", test_scrgb_neutrals_windows},
      {"sdl_level_guard", test_sdl_level_not_used_as_nits},
      {"above_one", test_above_one_scrgb},
      {"wide_gamut", test_wide_gamut_negative_channel},
      {"lut", test_lut_matches_direct},
      {"sdr_regression", test_sdr_regression_unchanged},
      {"hlg_ootf", test_hlg_ootf_reference_display},
      {"hlg_scrgb_neutral", test_hlg_rgb48_scrgb_neutral},
  };
  for (const auto& c : cases) {
    if (c.fn() != EXIT_SUCCESS) {
      std::cerr << "in " << c.name << "\n";
      return EXIT_FAILURE;
    }
  }
  std::cout << "test_hdr_color: OK\n";
  return EXIT_SUCCESS;
}