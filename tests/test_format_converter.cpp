#include "../format_converter.h"
#include "../frame_metadata.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
extern "C" {
#include <libavutil/imgutils.h>
}

static int failures = 0;

static void fail(const char* label) {
  std::fprintf(stderr, "FAIL %s\n", label);
  failures++;
}

static void pass(const char* label) {
  std::printf("PASS %s\n", label);
}

static AVFrame* alloc_frame(const int width, const int height, const AVPixelFormat format) {
  AVFrame* frame = av_frame_alloc();
  if (frame == nullptr) {
    throw std::runtime_error("av_frame_alloc failed");
  }
  frame->format = format;
  frame->width = width;
  frame->height = height;
  frame->colorspace = AVCOL_SPC_BT709;
  frame->color_range = AVCOL_RANGE_JPEG;
  frame->pts = 0;
  if (av_image_alloc(frame->data, frame->linesize, width, height, format, 64) < 0) {
    av_frame_free(&frame);
    throw std::runtime_error("av_image_alloc failed");
  }
  return frame;
}

static void free_frame(AVFrame* frame) {
  if (frame == nullptr) {
    return;
  }
  av_freep(&frame->data[0]);
  av_frame_free(&frame);
}

static void fill_rgb24_pattern(AVFrame* frame) {
  for (int y = 0; y < frame->height; ++y) {
    uint8_t* row = frame->data[0] + y * frame->linesize[0];
    for (int x = 0; x < frame->width; ++x) {
      row[x * 3 + 0] = static_cast<uint8_t>(x & 0xff);
      row[x * 3 + 1] = static_cast<uint8_t>(y & 0xff);
      row[x * 3 + 2] = 128;
    }
  }
}

static bool rgb24_pixel_eq(const AVFrame* frame, const int x, const int y, const int r, const int g, const int b) {
  const uint8_t* p = frame->data[0] + y * frame->linesize[0] + x * 3;
  return p[0] == r && p[1] == g && p[2] == b;
}

static bool rgb24_is_black(const AVFrame* frame, const int x, const int y) {
  return rgb24_pixel_eq(frame, x, y, 0, 0, 0);
}

static bool rgb48_is_black(const AVFrame* frame, const int x, const int y) {
  const uint16_t* p = reinterpret_cast<const uint16_t*>(frame->data[0] + y * frame->linesize[0] + x * 6);
  return p[0] == 0 && p[1] == 0 && p[2] == 0;
}

static bool rgb48_nonzero(const AVFrame* frame, const int x, const int y) {
  const uint16_t* p = reinterpret_cast<const uint16_t*>(frame->data[0] + y * frame->linesize[0] + x * 6);
  return p[0] != 0 || p[1] != 0 || p[2] != 0;
}

static void test_native_rgb24_centered() {
  const int src_w = 8;
  const int src_h = 6;
  const int canvas_w = 16;
  const int canvas_h = 12;
  FormatConverter converter(src_w, src_h, canvas_w, canvas_h, AV_PIX_FMT_RGB24, AV_PIX_FMT_RGB24, AVCOL_SPC_BT709, AVCOL_RANGE_JPEG, ConversionFit::Native, NONE, SWS_POINT);

  AVFrame* src = alloc_frame(src_w, src_h, AV_PIX_FMT_RGB24);
  AVFrame* dst = alloc_frame(canvas_w, canvas_h, AV_PIX_FMT_RGB24);
  fill_rgb24_pattern(src);
  std::memset(dst->data[0], 0xff, static_cast<size_t>(dst->linesize[0]) * canvas_h);

  converter(src, dst);

  const int x0 = (canvas_w - src_w) / 2;
  const int y0 = (canvas_h - src_h) / 2;
  bool ok = dst->width == canvas_w && dst->height == canvas_h;
  ok = ok && FrameMetadata::get_original_width(dst, 0) == src_w && FrameMetadata::get_original_height(dst, 0) == src_h;
  ok = ok && rgb24_is_black(dst, 0, 0) && rgb24_is_black(dst, canvas_w - 1, canvas_h - 1);
  ok = ok && rgb24_pixel_eq(dst, x0, y0, 0, 0, 128);
  ok = ok && rgb24_pixel_eq(dst, x0 + src_w - 1, y0 + src_h - 1, (src_w - 1) & 0xff, (src_h - 1) & 0xff, 128);

  if (ok) {
    pass("native RGB24 centered 1:1 with black padding");
  } else {
    fail("native RGB24 centered 1:1 with black padding");
  }

  free_frame(src);
  free_frame(dst);
}

static void test_native_rgb24_odd_offset_and_linesize() {
  const int src_w = 5;
  const int src_h = 4;
  const int canvas_w = 12;
  const int canvas_h = 8;
  FormatConverter converter(src_w, src_h, canvas_w, canvas_h, AV_PIX_FMT_RGB24, AV_PIX_FMT_RGB24, AVCOL_SPC_BT709, AVCOL_RANGE_JPEG, ConversionFit::Native, NONE, SWS_POINT);

  AVFrame* src = alloc_frame(src_w, src_h, AV_PIX_FMT_RGB24);
  AVFrame* dst = alloc_frame(canvas_w, canvas_h, AV_PIX_FMT_RGB24);
  fill_rgb24_pattern(src);
  std::memset(dst->data[0], 0xff, static_cast<size_t>(dst->linesize[0]) * canvas_h);

  converter(src, dst);

  const int x0 = (canvas_w - src_w) / 2;
  const int y0 = (canvas_h - src_h) / 2;
  bool ok = (x0 == 3) && (y0 == 2);
  ok = ok && dst->linesize[0] > src_w * 3;
  ok = ok && rgb24_is_black(dst, 0, 0) && rgb24_is_black(dst, x0 - 1, y0);
  ok = ok && rgb24_is_black(dst, x0 + src_w, y0) && rgb24_is_black(dst, canvas_w - 1, canvas_h - 1);
  for (int y = 0; y < src_h && ok; ++y) {
    for (int x = 0; x < src_w && ok; ++x) {
      ok = rgb24_pixel_eq(dst, x0 + x, y0 + y, x & 0xff, y & 0xff, 128);
    }
  }

  if (ok) {
    pass("native RGB24 odd horizontal offset uses canvas linesize");
  } else {
    fail("native RGB24 odd horizontal offset uses canvas linesize");
  }

  free_frame(src);
  free_frame(dst);
}

static void test_native_rgb48le() {
  const int src_w = 5;
  const int src_h = 5;
  const int canvas_w = 11;
  const int canvas_h = 9;
  FormatConverter converter(src_w, src_h, canvas_w, canvas_h, AV_PIX_FMT_RGB24, AV_PIX_FMT_RGB48LE, AVCOL_SPC_BT709, AVCOL_RANGE_JPEG, ConversionFit::Native, NONE, SWS_POINT);

  AVFrame* src = alloc_frame(src_w, src_h, AV_PIX_FMT_RGB24);
  AVFrame* dst = alloc_frame(canvas_w, canvas_h, AV_PIX_FMT_RGB48LE);
  fill_rgb24_pattern(src);

  converter(src, dst);

  const int x0 = (canvas_w - src_w) / 2;
  const int y0 = (canvas_h - src_h) / 2;
  bool ok = (x0 == 3) && (y0 == 2);
  ok = ok && dst->linesize[0] > src_w * 6;
  ok = ok && rgb48_is_black(dst, 0, 0) && rgb48_is_black(dst, canvas_w - 1, canvas_h - 1);
  ok = ok && rgb48_is_black(dst, x0 - 1, y0);
  ok = ok && rgb48_nonzero(dst, x0, y0);
  ok = ok && rgb48_nonzero(dst, x0 + src_w - 1, y0 + src_h - 1);
  ok = ok && FrameMetadata::get_original_width(dst, 0) == src_w;

  if (ok) {
    pass("native RGB48LE centered with black padding");
  } else {
    fail("native RGB48LE centered with black padding");
  }

  free_frame(src);
  free_frame(dst);
}

static void test_native_src_equals_canvas() {
  FormatConverter converter(4, 4, 4, 4, AV_PIX_FMT_RGB24, AV_PIX_FMT_RGB24, AVCOL_SPC_BT709, AVCOL_RANGE_JPEG, ConversionFit::Native, NONE, SWS_POINT);
  AVFrame* src = alloc_frame(4, 4, AV_PIX_FMT_RGB24);
  AVFrame* dst = alloc_frame(4, 4, AV_PIX_FMT_RGB24);
  fill_rgb24_pattern(src);
  converter(src, dst);

  bool ok = rgb24_pixel_eq(dst, 0, 0, 0, 0, 128) && rgb24_pixel_eq(dst, 3, 3, 3, 3, 128);
  if (ok) {
    pass("native src == canvas");
  } else {
    fail("native src == canvas");
  }
  free_frame(src);
  free_frame(dst);
}

static void test_native_src_change_still_fits() {
  FormatConverter converter(4, 4, 12, 10, AV_PIX_FMT_RGB24, AV_PIX_FMT_RGB24, AVCOL_SPC_BT709, AVCOL_RANGE_JPEG, ConversionFit::Native, NONE, SWS_POINT);
  AVFrame* src = alloc_frame(6, 6, AV_PIX_FMT_RGB24);
  AVFrame* dst = alloc_frame(12, 10, AV_PIX_FMT_RGB24);
  fill_rgb24_pattern(src);
  converter(src, dst);

  const int x0 = (12 - 6) / 2;
  const int y0 = (10 - 6) / 2;
  bool ok = rgb24_is_black(dst, 0, 0) && rgb24_pixel_eq(dst, x0, y0, 0, 0, 128);
  if (ok) {
    pass("native src size change still fitting canvas");
  } else {
    fail("native src size change still fitting canvas");
  }
  free_frame(src);
  free_frame(dst);
}

static void test_native_overflow_throws() {
  bool threw = false;
  try {
    FormatConverter converter(20, 10, 16, 16, AV_PIX_FMT_RGB24, AV_PIX_FMT_RGB24, AVCOL_SPC_BT709, AVCOL_RANGE_JPEG, ConversionFit::Native, NONE, SWS_POINT);
    (void)converter;
  } catch (const std::runtime_error& e) {
    threw = std::string(e.what()).find("does not fit conversion canvas") != std::string::npos;
  }

  if (!threw) {
    fail("native overflow at construction");
    return;
  }

  threw = false;
  FormatConverter converter(4, 4, 8, 8, AV_PIX_FMT_RGB24, AV_PIX_FMT_RGB24, AVCOL_SPC_BT709, AVCOL_RANGE_JPEG, ConversionFit::Native, NONE, SWS_POINT);
  AVFrame* src = alloc_frame(10, 4, AV_PIX_FMT_RGB24);
  AVFrame* dst = alloc_frame(8, 8, AV_PIX_FMT_RGB24);
  fill_rgb24_pattern(src);
  try {
    converter(src, dst);
  } catch (const std::runtime_error& e) {
    threw = std::string(e.what()).find("does not fit conversion canvas") != std::string::npos;
  }
  free_frame(src);
  free_frame(dst);

  if (threw) {
    pass("native overflow throws");
  } else {
    fail("native overflow on src growth");
  }
}

static void test_stretch_fills_canvas() {
  FormatConverter converter(4, 4, 8, 8, AV_PIX_FMT_RGB24, AV_PIX_FMT_RGB24, AVCOL_SPC_BT709, AVCOL_RANGE_JPEG, ConversionFit::Stretch, NONE, SWS_POINT);
  AVFrame* src = alloc_frame(4, 4, AV_PIX_FMT_RGB24);
  AVFrame* dst = alloc_frame(8, 8, AV_PIX_FMT_RGB24);
  fill_rgb24_pattern(src);
  converter(src, dst);

  bool ok = !rgb24_is_black(dst, 0, 0) && !rgb24_is_black(dst, 7, 7);
  if (ok) {
    pass("stretch fills canvas");
  } else {
    fail("stretch fills canvas");
  }
  free_frame(src);
  free_frame(dst);
}

int main() {
  test_native_rgb24_centered();
  test_native_rgb24_odd_offset_and_linesize();
  test_native_rgb48le();
  test_native_src_equals_canvas();
  test_native_src_change_still_fits();
  test_native_overflow_throws();
  test_stretch_fills_canvas();

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }

  std::printf("All format converter tests passed\n");
  return EXIT_SUCCESS;
}
