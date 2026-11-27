#include "format_converter.h"
#include "hdr_color.h"
#include "hdr_metadata.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr int kW = 16;
constexpr int kH = 8;
constexpr float kRef = hdr_color::kWindowsScrgbContentWhiteNits;

bool near(float a, float b, float eps) {
  return std::fabs(a - b) <= eps;
}

int fail(const std::string& msg) {
  std::cerr << "FAIL: " << msg << "\n";
  return EXIT_FAILURE;
}

uint16_t pq_sample_from_nits(float nits) {
  const float pq = hdr_color::linear_nits_to_pq_signal(nits);
  return static_cast<uint16_t>(std::lround(std::min(1.0F, std::max(0.0F, pq)) * 65535.0F));
}

void fill_rgb48_patch(AVFrame* frame, const int x, const int y, const float r_nits, const float g_nits, const float b_nits) {
  uint16_t* row = reinterpret_cast<uint16_t*>(frame->data[0] + y * frame->linesize[0]);
  row[x * 3 + 0] = pq_sample_from_nits(r_nits);
  row[x * 3 + 1] = pq_sample_from_nits(g_nits);
  row[x * 3 + 2] = pq_sample_from_nits(b_nits);
}

void fill_solid_rect_rgb48(AVFrame* frame, const int x0, const int y0, const int x1, const int y1, const float r_nits, const float g_nits, const float b_nits) {
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      fill_rgb48_patch(frame, x, y, r_nits, g_nits, b_nits);
    }
  }
}

std::array<float, 3> scrgb_at(const AVFrame* frame, const int x, const int y) {
  const uint16_t* row = reinterpret_cast<const uint16_t*>(frame->data[0] + y * frame->linesize[0]);
  const uint16_t* px = row + x * 3;
  return hdr_color::hdr_rgb48_full_range_pixel_to_scrgb_linear(px[0], px[1], px[2], AVCOL_TRC_SMPTE2084, kRef);
}

std::array<float, 3> expected_nits_at(const AVFrame* ref, const int x, const int y) {
  const uint16_t* row = reinterpret_cast<const uint16_t*>(ref->data[0] + y * ref->linesize[0]);
  const uint16_t* px = row + x * 3;
  return {
      hdr_color::pq_rgb48_full_range_sample_to_nits(px[0]),
      hdr_color::pq_rgb48_full_range_sample_to_nits(px[1]),
      hdr_color::pq_rgb48_full_range_sample_to_nits(px[2]),
  };
}

bool encode_rgb48_to_yuv420p10_mpeg(const AVFrame* src, AVFrame* dst) {
  SwsContext* ctx = sws_getContext(src->width, src->height, AV_PIX_FMT_RGB48LE, dst->width, dst->height, AV_PIX_FMT_YUV420P10LE, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (ctx == nullptr) {
    return false;
  }
  const int* coeffs = sws_getCoefficients(SWS_CS_BT2020);
  sws_setColorspaceDetails(ctx, coeffs, 1, coeffs, 0, 0, 1 << 16, 1 << 16);
  const int ret = sws_scale(ctx, src->data, src->linesize, 0, src->height, dst->data, dst->linesize);
  sws_freeContext(ctx);
  if (ret <= 0) {
    return false;
  }
  dst->color_range = AVCOL_RANGE_MPEG;
  dst->color_primaries = AVCOL_PRI_BT2020;
  dst->color_trc = AVCOL_TRC_SMPTE2084;
  dst->colorspace = AVCOL_SPC_BT2020_NCL;
  return true;
}

int sample_patch(const AVFrame* rgb48, const int label_x) {
  constexpr int y = 3;
  const auto scrgb = scrgb_at(rgb48, label_x, y);
  const auto nits = expected_nits_at(rgb48, label_x, y);

  if (label_x == 2) {
    if (scrgb[0] > 0.04F || scrgb[1] > 0.04F || scrgb[2] > 0.04F) {
      return fail("PQ black patch scrgb too high after YUV limited round-trip");
    }
    if (nits[0] > 1.0F || nits[1] > 1.0F || nits[2] > 1.0F) {
      return fail("PQ black patch nits too high");
    }
    return EXIT_SUCCESS;
  }
  if (label_x == 7) {
    const float expected = 100.0F / kRef;
    if (!near(scrgb[0], expected, 0.12F) || !near(scrgb[1], expected, 0.12F) || !near(scrgb[2], expected, 0.12F)) {
      return fail("PQ 100-nit neutral patch scRGB mismatch");
    }
    if (!near(nits[0], 100.0F, 8.0F) || !near(nits[1], 100.0F, 8.0F) || !near(nits[2], 100.0F, 8.0F)) {
      return fail("PQ neutral patch absolute nits mismatch");
    }
    return EXIT_SUCCESS;
  }
  if (label_x == 12) {
    if (!near(nits[0], 450.0F, 40.0F) || !near(nits[1], 95.0F, 20.0F) || !near(nits[2], 75.0F, 18.0F)) {
      return fail("PQ colored patch absolute nits mismatch");
    }
    if (scrgb[0] <= scrgb[1] || scrgb[0] <= 0.5F) {
      return fail("PQ colored patch expected dominant red scRGB channel");
    }
    return EXIT_SUCCESS;
  }
  return EXIT_SUCCESS;
}

}  // namespace

int main() {
  hdr_color::ensure_pq_rgb48_lut_initialized();

  AVFrame* reference_rgb48 = av_frame_alloc();
  reference_rgb48->width = kW;
  reference_rgb48->height = kH;
  reference_rgb48->format = AV_PIX_FMT_RGB48LE;
  reference_rgb48->color_primaries = AVCOL_PRI_BT2020;
  reference_rgb48->color_trc = AVCOL_TRC_SMPTE2084;
  reference_rgb48->colorspace = AVCOL_SPC_BT2020_NCL;
  reference_rgb48->color_range = AVCOL_RANGE_JPEG;
  if (av_frame_get_buffer(reference_rgb48, 32) < 0) {
    return fail("reference buffer");
  }

  fill_solid_rect_rgb48(reference_rgb48, 0, 0, 4, 6, 0.0F, 0.0F, 0.0F);
  fill_solid_rect_rgb48(reference_rgb48, 5, 0, 9, 6, 100.0F, 100.0F, 100.0F);
  fill_solid_rect_rgb48(reference_rgb48, 10, 0, 15, 6, 450.0F, 95.0F, 75.0F);

  AVFrame* yuv_limited = av_frame_alloc();
  yuv_limited->width = kW;
  yuv_limited->height = kH;
  yuv_limited->format = AV_PIX_FMT_YUV420P10LE;
  if (av_image_alloc(yuv_limited->data, yuv_limited->linesize, kW, kH, AV_PIX_FMT_YUV420P10LE, 32) < 0) {
    return fail("yuv buffer");
  }
  if (!encode_rgb48_to_yuv420p10_mpeg(reference_rgb48, yuv_limited)) {
    return fail("RGB48 -> YUV420P10LE MPEG limited encode");
  }

  if (yuv_limited->color_range != AVCOL_RANGE_MPEG) {
    return fail("fixture YUV must be MPEG limited range");
  }
  if (yuv_limited->color_trc != AVCOL_TRC_SMPTE2084 || yuv_limited->colorspace != AVCOL_SPC_BT2020_NCL) {
    return fail("fixture YUV color metadata");
  }

  FormatConverter converter(kW, kH, kW, kH, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_RGB48LE, AVCOL_SPC_BT2020_NCL, AVCOL_RANGE_MPEG, ConversionFit::Stretch, NONE, SWS_BILINEAR, true);

  AVFrame* dst = av_frame_alloc();
  if (av_image_alloc(dst->data, dst->linesize, kW, kH, AV_PIX_FMT_RGB48LE, 32) < 0) {
    return fail("dst buffer");
  }
  dst->width = kW;
  dst->height = kH;
  dst->format = AV_PIX_FMT_RGB48LE;

  converter(yuv_limited, dst);

  try {
    hdr_metadata::validate_hdr_converted_frame("fixture", *dst);
  } catch (const std::exception& e) {
    return fail(std::string("validate_hdr_converted_frame: ") + e.what());
  }

  if (dst->color_trc != AVCOL_TRC_SMPTE2084) {
    return fail("HDR conversion must preserve PQ transfer");
  }

  for (const int patch_x : {2, 7, 12}) {
    if (sample_patch(dst, patch_x) != EXIT_SUCCESS) {
      return EXIT_FAILURE;
    }
  }

  // Range lie: treating limited YUV as full must not reproduce neutrals.
  yuv_limited->color_range = AVCOL_RANGE_JPEG;
  FormatConverter wrong_range_converter(kW, kH, kW, kH, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_RGB48LE, AVCOL_SPC_BT2020_NCL, AVCOL_RANGE_JPEG, ConversionFit::Stretch, NONE, SWS_BILINEAR, true);
  AVFrame* wrong = av_frame_alloc();
  if (av_image_alloc(wrong->data, wrong->linesize, kW, kH, AV_PIX_FMT_RGB48LE, 32) < 0) {
    return fail("wrong dst buffer");
  }
  wrong->width = kW;
  wrong->height = kH;
  wrong->format = AV_PIX_FMT_RGB48LE;
  wrong_range_converter(yuv_limited, wrong);
  const auto wrong_scrgb = scrgb_at(wrong, 7, 3);
  const float expected_neutral = 100.0F / kRef;
  if (near(wrong_scrgb[0], expected_neutral, 0.08F)) {
    return fail("range metadata mismatch must not yield correct neutral scRGB");
  }

  av_frame_free(&reference_rgb48);
  av_freep(&yuv_limited->data[0]);
  av_frame_free(&yuv_limited);
  av_freep(&dst->data[0]);
  av_frame_free(&dst);
  av_freep(&wrong->data[0]);
  av_frame_free(&wrong);

  std::cout << "test_hdr_yuv_fixture: OK\n";
  return EXIT_SUCCESS;
}