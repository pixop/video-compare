#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

// Reuse video frame pixel storage while AVFrame/AVBufferRef retain independent
// ownership. Each instance manages one image layout at a time.
class VideoFrameBufferPool {
 public:
  VideoFrameBufferPool() = default;
  VideoFrameBufferPool(const VideoFrameBufferPool&) = delete;
  VideoFrameBufferPool& operator=(const VideoFrameBufferPool&) = delete;
  ~VideoFrameBufferPool() { reset(); }

  // Retire cached buffers. Outstanding frames remain valid until their last
  // AVBufferRef is released.
  void reset() { av_buffer_pool_uninit(&pool_); }

  void allocate(AVFrame* frame, const int width, const int height, const AVPixelFormat format) {
    static constexpr int kAlignment = 64;
    if (frame == nullptr || frame->buf[0] != nullptr) {
      throw std::runtime_error("Video frame must be allocated and have no pixel buffer");
    }
    const int image_bytes = av_image_get_buffer_size(format, width, height, kAlignment);
    if (image_bytes <= 0 || image_bytes > std::numeric_limits<int>::max() - (kAlignment - 1)) {
      throw std::runtime_error("Invalid video frame buffer dimensions");
    }

    if (pool_ == nullptr || width != width_ || height != height_ || format != format_) {
      // Existing frames can outlive their pool: FFmpeg retains the pool until
      // its last outstanding AVBufferRef has been released.
      av_buffer_pool_uninit(&pool_);
      pool_ = av_buffer_pool_init(image_bytes + kAlignment - 1, nullptr);
      if (pool_ == nullptr) {
        throw std::runtime_error("Could not initialize video frame buffer pool");
      }
      width_ = width;
      height_ = height;
      format_ = format;
    }

    AVBufferRef* buffer = av_buffer_pool_get(pool_);
    if (buffer == nullptr) {
      throw std::runtime_error("Could not get video frame buffer from pool");
    }

    // av_buffer_alloc() guarantees FFmpeg's minimum alignment, not necessarily
    // 64 bytes on every platform. Reserve extra bytes and align the image start.
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(buffer->data);
    const std::size_t offset = (kAlignment - (address % kAlignment)) % kAlignment;
    const int used = av_image_fill_arrays(frame->data, frame->linesize, buffer->data + offset, format, width, height, kAlignment);
    if (used <= 0 || static_cast<std::size_t>(used) > buffer->size - offset) {
      av_buffer_unref(&buffer);
      throw std::runtime_error("Could not prepare video frame buffer layout");
    }
    frame->buf[0] = buffer;
  }

 private:
  AVBufferPool* pool_{nullptr};
  int width_{0};
  int height_{0};
  AVPixelFormat format_{AV_PIX_FMT_NONE};
};
