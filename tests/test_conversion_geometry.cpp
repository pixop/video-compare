#include "conversion_geometry.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

static int failures = 0;

static void expect_rect(const char* label, const ContentRect& got, const int x, const int y, const int w, const int h) {
  if (got.x != x || got.y != y || got.width != w || got.height != h) {
    std::fprintf(stderr, "FAIL %s: expected %d,%d %dx%d, got %d,%d %dx%d\n", label, x, y, w, h, got.x, got.y, got.width, got.height);
    failures++;
    return;
  }
  std::printf("PASS %s -> %d,%d %dx%d\n", label, got.x, got.y, got.width, got.height);
}

static void expect_mapped(const char* label, const MappedContentRect& got, const bool valid, const int x, const int y, const int w, const int h) {
  if (got.valid != valid) {
    std::fprintf(stderr, "FAIL %s: expected valid=%d, got valid=%d\n", label, static_cast<int>(valid), static_cast<int>(got.valid));
    failures++;
    return;
  }
  if (!valid) {
    std::printf("PASS %s -> invalid\n", label);
    return;
  }
  if (got.x != x || got.y != y || got.width != w || got.height != h) {
    std::fprintf(stderr, "FAIL %s: expected %d,%d %dx%d, got %d,%d %dx%d\n", label, x, y, w, h, got.x, got.y, got.width, got.height);
    failures++;
    return;
  }
  std::printf("PASS %s -> %d,%d %dx%d\n", label, got.x, got.y, got.width, got.height);
}

static void expect_point(const char* label, const bool ok, const int got_x, const int got_y, const bool expect_ok, const int expect_x, const int expect_y) {
  if (ok != expect_ok) {
    std::fprintf(stderr, "FAIL %s: expected ok=%d, got ok=%d\n", label, static_cast<int>(expect_ok), static_cast<int>(ok));
    failures++;
    return;
  }
  if (!expect_ok) {
    std::printf("PASS %s -> padding\n", label);
    return;
  }
  if (got_x != expect_x || got_y != expect_y) {
    std::fprintf(stderr, "FAIL %s: expected %d,%d, got %d,%d\n", label, expect_x, expect_y, got_x, got_y);
    failures++;
    return;
  }
  std::printf("PASS %s -> %d,%d\n", label, got_x, got_y);
}

int main() {
  expect_rect("native 1920x1440 / 1440x1080", content_rect_in_canvas(ConversionFit::Native, 1920, 1440, 1440, 1080), 240, 180, 1440, 1080);
  expect_rect("native 1920x1080 / 1280x1080", content_rect_in_canvas(ConversionFit::Native, 1920, 1080, 1280, 1080), 320, 0, 1280, 1080);
  expect_rect("native odd width extra on right", content_rect_in_canvas(ConversionFit::Native, 101, 100, 100, 100), 0, 0, 100, 100);
  expect_rect("native odd height extra on bottom", content_rect_in_canvas(ConversionFit::Native, 100, 101, 100, 100), 0, 0, 100, 100);
  expect_rect("native odd both", content_rect_in_canvas(ConversionFit::Native, 11, 11, 4, 4), 3, 3, 4, 4);
  expect_rect("native odd width extra pixel on right", content_rect_in_canvas(ConversionFit::Native, 10, 8, 3, 8), 3, 0, 3, 8);
  expect_rect("native oversized does not produce negative coords", content_rect_in_canvas(ConversionFit::Native, 1920, 1080, 2048, 1080), 0, 0, 0, 0);
  expect_rect("stretch fills canvas", content_rect_in_canvas(ConversionFit::Stretch, 1920, 1440, 1440, 1080), 0, 0, 1920, 1440);

  const CanvasRect inside{240, 180, 1440, 1080};
  expect_mapped("native crop fully inside", canvas_selection_to_content_crop(ConversionFit::Native, 1920, 1440, 1440, 1080, inside), true, 0, 0, 1440, 1080);

  const CanvasRect partial{0, 0, 400, 400};
  expect_mapped("native crop partial padding", canvas_selection_to_content_crop(ConversionFit::Native, 1920, 1440, 1440, 1080, partial), true, 0, 0, 160, 220);

  const CanvasRect pad_only{0, 0, 100, 100};
  expect_mapped("native crop padding only", canvas_selection_to_content_crop(ConversionFit::Native, 1920, 1440, 1440, 1080, pad_only), false, 0, 0, 0, 0);

  const CanvasRect stretch_sel{0, 0, 1920, 1080};
  expect_mapped("stretch crop scales", canvas_selection_to_content_crop(ConversionFit::Stretch, 1920, 1080, 1280, 720, stretch_sel), true, 0, 0, 1280, 720);

  const CanvasRect oversized_sel{0, 0, 100, 100};
  expect_mapped("native crop with oversized content is invalid", canvas_selection_to_content_crop(ConversionFit::Native, 1920, 1080, 2048, 1080, oversized_sel), false, 0, 0, 0, 0);

  // Stretch crop in video_compare.cpp uses the pre-helper formula: llround, then max(kMinCropDimension, ...).
  // A 1px canvas selection against much smaller content used to round to 0 and then be lifted to 2.
  {
    static constexpr int kMinCropDimension = 2;
    const int canvas_w = 1920;
    const int canvas_h = 1080;
    const int content_w = 640;
    const int content_h = 360;
    const int selection_w = 1;
    const int selection_h = 1;
    const int mapped_w = std::max(kMinCropDimension, static_cast<int>(std::llround(static_cast<double>(selection_w) * content_w / canvas_w)));
    const int mapped_h = std::max(kMinCropDimension, static_cast<int>(std::llround(static_cast<double>(selection_h) * content_h / canvas_h)));
    if (mapped_w != 2 || mapped_h != 2) {
      std::fprintf(stderr, "FAIL stretch 1px selection remains min 2x2: got %dx%d\n", mapped_w, mapped_h);
      failures++;
    } else {
      std::printf("PASS stretch 1px selection remains min 2x2\n");
    }
  }

  int cx = -1;
  int cy = -1;
  bool ok = canvas_point_to_content(ConversionFit::Native, 1920, 1440, 1440, 1080, 240, 180, &cx, &cy);
  expect_point("native point in content", ok, cx, cy, true, 0, 0);
  ok = canvas_point_to_content(ConversionFit::Native, 1920, 1440, 1440, 1080, 0, 0, &cx, &cy);
  expect_point("native point in padding", ok, cx, cy, false, 0, 0);
  ok = canvas_point_to_content(ConversionFit::Native, 1920, 1080, 2048, 1080, 0, 0, &cx, &cy);
  expect_point("native point with oversized content", ok, cx, cy, false, 0, 0);
  ok = canvas_point_to_content(ConversionFit::Stretch, 1920, 1080, 1280, 720, 960, 540, &cx, &cy);
  expect_point("stretch point scales", ok, cx, cy, true, 640, 360);

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }

  std::printf("All conversion geometry tests passed\n");
  return EXIT_SUCCESS;
}
