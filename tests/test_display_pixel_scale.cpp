#include "display_pixel_scale.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using display_pixel_scale::PhysicalPixelScale;
using display_pixel_scale::compute_physical_pixel_scale;
using display_pixel_scale::physical_1to1_viewport_zoom_base;
using display_pixel_scale::zoom_preset_viewport_factor;

static int failures = 0;

static void expect_true(const char* label, const bool got) {
  if (!got) {
    std::fprintf(stderr, "FAIL %s\n", label);
    failures++;
    return;
  }
  std::printf("PASS %s\n", label);
}

static void expect_near(const char* label, const float got, const float expected) {
  if (std::fabs(got - expected) > 1.0e-4F) {
    std::fprintf(stderr, "FAIL %s: expected %.6f, got %.6f\n", label, expected, got);
    failures++;
    return;
  }
  std::printf("PASS %s -> %.6f\n", label, got);
}

static void expect_scale_pair(const char* label, const PhysicalPixelScale& got, const float expected_x, const float expected_y) {
  expect_near(label, got.x, expected_x);
  expect_near(std::string(std::string(label) + " y").c_str(), got.y, expected_y);
}

static void expect_substr(const char* label, const std::string& haystack, const std::string& needle) {
  if (haystack.find(needle) == std::string::npos) {
    std::fprintf(stderr, "FAIL %s: missing %s in %s\n", label, needle.c_str(), haystack.c_str());
    failures++;
    return;
  }
  std::printf("PASS %s\n", label);
}

static void test_uniform_1x_and_2x_dpi() {
  // Stretch: layout fills content; video_to_window == layout/content (here 1.0).
  const PhysicalPixelScale one_x = compute_physical_pixel_scale(1.0F, 1.0F, 1.0F, 1.0F, 1.0F);
  expect_scale_pair("1x DPI at x1 zoom", one_x, 1.0F, 1.0F);

  const PhysicalPixelScale two_x = compute_physical_pixel_scale(1.0F, 2.0F, 2.0F, 1.0F, 1.0F);
  expect_scale_pair("2x DPI at x1 zoom", two_x, 2.0F, 2.0F);

  const float zoom_shift6 = zoom_preset_viewport_factor(2, true, 1.0F, 2.0F, 1.0F, 2.0F);
  expect_near("Shift+6 viewport on 2x DPI (base)", physical_1to1_viewport_zoom_base(1.0F, 2.0F, 1.0F, 2.0F), 0.5F);
  expect_near("Shift+6 viewport on 2x DPI", zoom_shift6, 0.5F);
  const PhysicalPixelScale after_shift6 = compute_physical_pixel_scale(zoom_shift6, 2.0F, 2.0F, 1.0F, 1.0F);
  expect_scale_pair("2x DPI Shift+6 physical 100%", after_shift6, 1.0F, 1.0F);
}

static void test_letterboxed_aspect() {
  // Unequal video_to_window factors (as after aspect fitting): layout width maps
  // across a narrower content viewport than height (e.g. pillarbox-style X/Y mismatch).
  const float video_x = 1920.0F / 1440.0F;
  const float video_y = 1.0F;
  const PhysicalPixelScale at_x1 = compute_physical_pixel_scale(1.0F, 1.0F, 1.0F, video_x, video_y);
  expect_near("letterbox scale_x", at_x1.x, 0.75F);
  expect_near("letterbox scale_y", at_x1.y, 1.0F);

  const float zoom_shift6_letterbox = zoom_preset_viewport_factor(2, true, video_x, 1.0F, video_y, 1.0F);
  expect_near("letterbox Shift+6 viewport", zoom_shift6_letterbox, 1.0F);
  const PhysicalPixelScale after_shift6_letterbox = compute_physical_pixel_scale(zoom_shift6_letterbox, 1.0F, 1.0F, video_x, video_y);
  expect_scale_pair("letterbox Shift+6 physical 100%", after_shift6_letterbox, 0.75F, 1.0F);
}

static void test_nonuniform_dpi() {
  const PhysicalPixelScale scale = compute_physical_pixel_scale(1.0F, 2.0F, 3.0F, 1.0F, 1.0F);
  expect_scale_pair("non-uniform DPI", scale, 2.0F, 3.0F);
}

static void test_viewport_zoom_presets() {
  static const float k_expected[6] = {0.25F, 0.5F, 1.0F, 2.0F, 4.0F, 8.0F};
  for (int i = 0; i < 6; ++i) {
    expect_near(("plain key " + std::to_string(4 + i) + " viewport").c_str(), zoom_preset_viewport_factor(i, false, 1.0F, 1.0F, 1.0F, 1.0F), k_expected[i]);
  }
}

static void test_shift_physical_zoom_presets() {
  const float zoom_shift6_2x = zoom_preset_viewport_factor(2, true, 1.0F, 2.0F, 1.0F, 2.0F);
  expect_near("Shift+6 viewport on 2x DPI", zoom_shift6_2x, 0.5F);
  const PhysicalPixelScale shift6_phys = compute_physical_pixel_scale(zoom_shift6_2x, 2.0F, 2.0F, 1.0F, 1.0F);
  expect_scale_pair("Shift+6 physical 100% on 2x DPI", shift6_phys, 1.0F, 1.0F);

  const float video_x = 1920.0F / 1440.0F;
  const float zoom_shift6_letterbox = zoom_preset_viewport_factor(2, true, video_x, 1.0F, 1.0F, 1.0F);
  expect_near("Shift+6 letterbox viewport", zoom_shift6_letterbox, 1.0F);
  const PhysicalPixelScale shift6_letterbox_phys = compute_physical_pixel_scale(zoom_shift6_letterbox, 1.0F, 1.0F, video_x, 1.0F);
  expect_scale_pair("Shift+6 letterbox physical", shift6_letterbox_phys, 0.75F, 1.0F);

  const float zoom_shift4_2x = zoom_preset_viewport_factor(0, true, 1.0F, 2.0F, 1.0F, 2.0F);
  expect_near("Shift+4 viewport on 2x DPI", zoom_shift4_2x, 0.125F);
  const PhysicalPixelScale shift4_phys = compute_physical_pixel_scale(zoom_shift4_2x, 2.0F, 2.0F, 1.0F, 1.0F);
  expect_scale_pair("Shift+4 physical 25% on 2x DPI", shift4_phys, 0.25F, 0.25F);

  const float zoom_shift8 = zoom_preset_viewport_factor(4, true, 1.0F, 2.0F, 1.0F, 3.0F);
  expect_near("Shift+8 non-uniform DPI viewport", zoom_shift8, 4.0F / 3.0F);
  const PhysicalPixelScale shift8_phys = compute_physical_pixel_scale(zoom_shift8, 2.0F, 3.0F, 1.0F, 1.0F);
  expect_scale_pair("Shift+8 non-uniform DPI physical", shift8_phys, 8.0F / 3.0F, 4.0F);
}

static void test_append_formatting() {
  std::string message = "Display state: window=800x600";
  display_pixel_scale::append_display_state_viewport_and_pixel_scale(message, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F);
  expect_substr("uniform zoom field", message, " zoom=x1");
  expect_substr("uniform pixel_scale", message, " pixel_scale=100%");

  message = "Display state: window=800x600";
  display_pixel_scale::append_display_state_viewport_and_pixel_scale(message, 2.0F, 1.0F, 1.0F, 1.0F, 1.0F);
  expect_substr("x2 zoom", message, " zoom=x2");
  expect_substr("200% pixel_scale", message, " pixel_scale=200%");

  message = "Display state: window=1920x1080";
  display_pixel_scale::append_display_state_viewport_and_pixel_scale(message, 1.0F, 1.0F, 1.0F, 1920.0F / 1440.0F, 1.0F);
  expect_substr("split axis fields", message, " pixel_scale_x=75%");
  expect_substr("split axis fields y", message, " pixel_scale_y=100%");
}

int main() {
  test_uniform_1x_and_2x_dpi();
  test_letterboxed_aspect();
  test_nonuniform_dpi();
  test_viewport_zoom_presets();
  test_shift_physical_zoom_presets();
  test_append_formatting();

  if (failures != 0) {
    std::fprintf(stderr, "%d test(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("All display_pixel_scale tests passed\n");
  return EXIT_SUCCESS;
}
