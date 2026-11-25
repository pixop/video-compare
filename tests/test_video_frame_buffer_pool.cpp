#include "video_frame_buffer_pool.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
extern "C" {
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

static int failures = 0;

static void check(const bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s\n", message);
    ++failures;
  }
}

static AVFrame* alloc_frame() {
  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    throw std::runtime_error("Could not allocate AVFrame");
  }
  return frame;
}

static AVFrame* alloc_direct(const int width, const int height, const AVPixelFormat format) {
  AVFrame* frame = alloc_frame();
  if (av_image_alloc(frame->data, frame->linesize, width, height, format, 64) < 0) {
    av_frame_free(&frame);
    throw std::runtime_error("Could not allocate direct image");
  }
  return frame;
}

static void free_direct(AVFrame** frame) {
  av_freep(&(*frame)->data[0]);
  av_frame_free(frame);
}

static void test_reuse(const AVPixelFormat format) {
  VideoFrameBufferPool pool;
  AVFrame* first = alloc_frame();
  AVFrame* second = alloc_frame();
  pool.allocate(first, 191, 107, format);
  pool.allocate(second, 191, 107, format);
  const uint8_t* first_address = first->data[0];

  check(first->buf[0] != nullptr && second->buf[0] != nullptr, "buffers have FFmpeg references");
  check(first->data[0] != second->data[0], "live frames cannot alias");
  check(reinterpret_cast<uintptr_t>(first->data[0]) % 64 == 0, "first frame is 64-byte aligned");
  check(reinterpret_cast<uintptr_t>(second->data[0]) % 64 == 0, "second frame is 64-byte aligned");
  check(first->linesize[0] % 64 == 0, "row stride remains 64-byte aligned");

  std::memset(first->data[0], 0x21, first->linesize[0] * 107);
  std::memset(second->data[0], 0x42, second->linesize[0] * 107);
  av_frame_free(&first);
  AVFrame* recycled = alloc_frame();
  pool.allocate(recycled, 191, 107, format);
  check(recycled->data[0] == first_address, "freed image allocation is reused");
  check(second->data[0][0] == 0x42 && second->data[0][second->linesize[0] * 53] == 0x42, "recycling does not alter live frames");
  av_frame_free(&second);
  av_frame_free(&recycled);
}

static void test_reconfigure_and_outstanding_frames() {
  AVFrame* old = alloc_frame();
  AVFrame* next = alloc_frame();
  {
    VideoFrameBufferPool pool;
    pool.allocate(old, 128, 72, AV_PIX_FMT_RGB24);
    std::memset(old->data[0], 0x37, old->linesize[0] * 72);
    pool.allocate(next, 320, 241, AV_PIX_FMT_RGB48LE);
    check(next->linesize[0] % 64 == 0, "new format stride aligned");
    check(old->data[0][0] == 0x37 && old->data[0][old->linesize[0] * 71] == 0x37, "old pool remains valid after reconfiguration");
    av_frame_free(&next);
  }
  check(old->data[0][0] == 0x37 && old->data[0][old->linesize[0] * 71] == 0x37, "outstanding frame survives pool destruction");
  av_frame_free(&old);
}

static void test_reset_with_outstanding_frames() {
  VideoFrameBufferPool pool;
  AVFrame* retained = alloc_frame();
  AVFrame* released = alloc_frame();
  pool.allocate(retained, 320, 180, AV_PIX_FMT_RGB48LE);
  pool.allocate(released, 320, 180, AV_PIX_FMT_RGB48LE);
  std::memset(retained->data[0], 0x5a, retained->linesize[0] * 180);
  av_frame_free(&released);

  // Retire both in-use and cached buffers, as happens at EOF or during a seek.
  pool.reset();
  pool.reset();  // Repeated idle resets are safe.
  check(retained->data[0][0] == 0x5a && retained->data[0][retained->linesize[0] * 179] == 0x5a, "retained frame stays valid after reset");

  AVFrame* next = alloc_frame();
  pool.allocate(next, 320, 180, AV_PIX_FMT_RGB48LE);
  check(next->buf[0] != nullptr && next->data[0] != retained->data[0], "new pool must not reuse a live frame from the retired pool");
  std::memset(next->data[0], 0x34, next->linesize[0] * 180);
  check(retained->data[0][0] == 0x5a, "new pool writes do not affect retired frames");
  av_frame_free(&retained);
  check(next->data[0][0] == 0x34, "new pool frame survives release of retired frame");
  av_frame_free(&next);
  pool.reset();
}

static void test_conversion_pixels(const AVPixelFormat source_format, const AVPixelFormat dest_format) {
  const int width = 192;
  const int height = 110;
  AVFrame* src = alloc_direct(width, height, source_format);
  AVFrame* direct = alloc_direct(width, height, dest_format);
  AVFrame* pooled = alloc_frame();
  VideoFrameBufferPool pool;
  pool.allocate(pooled, width, height, dest_format);
  check(direct->linesize[0] == pooled->linesize[0], "pooled and direct strides match");

  const bool is_10bit = source_format == AV_PIX_FMT_YUV420P10LE;
  for (int plane = 0; plane < 3; ++plane) {
    const int plane_height = plane == 0 ? height : height / 2;
    const int plane_width = plane == 0 ? width : width / 2;
    for (int y = 0; y < plane_height; ++y) {
      for (int x = 0; x < plane_width; ++x) {
        const int value = (x * 3 + y * 7 + plane * 41) & (is_10bit ? 1023 : 255);
        if (is_10bit) {
          auto* row = reinterpret_cast<uint16_t*>(src->data[plane] + y * src->linesize[plane]);
          row[x] = static_cast<uint16_t>(value);
        } else {
          src->data[plane][y * src->linesize[plane] + x] = static_cast<uint8_t>(value);
        }
      }
    }
  }

  SwsContext* sws = sws_getContext(width, height, source_format, width, height, dest_format, SWS_BICUBIC | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
  if (!sws) {
    throw std::runtime_error("Could not initialize swscale");
  }
  const int direct_rows = sws_scale(sws, src->data, src->linesize, 0, height, direct->data, direct->linesize);
  const int pooled_rows = sws_scale(sws, src->data, src->linesize, 0, height, pooled->data, pooled->linesize);
  check(direct_rows == height && pooled_rows == height, "both conversions completed");
  for (int y = 0; y < height; ++y) {
    const int active_bytes = width * (dest_format == AV_PIX_FMT_RGB24 ? 3 : 6);
    check(std::memcmp(direct->data[0] + y * direct->linesize[0], pooled->data[0] + y * pooled->linesize[0], active_bytes) == 0, "converted pixels are identical");
  }

  sws_freeContext(sws);
  av_frame_free(&pooled);
  free_direct(&direct);
  free_direct(&src);
}

int main() {
  try {
    test_reuse(AV_PIX_FMT_RGB24);
    test_reuse(AV_PIX_FMT_RGB48LE);
    test_reconfigure_and_outstanding_frames();
    test_reset_with_outstanding_frames();
    test_conversion_pixels(AV_PIX_FMT_YUV420P, AV_PIX_FMT_RGB24);
    test_conversion_pixels(AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_RGB48LE);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL video frame buffer pool tests: %s\n", e.what());
    return 1;
  }
  if (failures != 0) {
    std::fprintf(stderr, "%d video frame buffer pool assertions failed\n", failures);
    return 1;
  }
  std::puts("PASS video frame buffer pool unit tests");
  return 0;
}
