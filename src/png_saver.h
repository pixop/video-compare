#pragma once
#include <memory>
#include <stdexcept>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

class PngSaver {
 public:
  class EncodingException : public std::runtime_error {
   public:
    explicit EncodingException(const std::string& message) : std::runtime_error(message) {}
  };

  class IOException : public std::runtime_error {
   public:
    explicit IOException(const std::string& message) : std::runtime_error(message) {}
  };

  static void save(const AVFrame* frame, const std::string& filename);

 private:
  struct AVFrameDeleter {
    void operator()(AVFrame* frame) const {
      if (frame) {
        av_freep(&frame->data[0]);
        av_frame_free(&frame);
      }
    }
  };

  struct AVCodecContextDeleter {
    void operator()(AVCodecContext* codec_ctx) const {
      if (codec_ctx) {
        avcodec_free_context(&codec_ctx);
      }
    }
  };

  struct AVPacketDeleter {
    void operator()(AVPacket* packet) const {
      if (packet) {
        av_packet_free(&packet);
      }
    }
  };

  using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;
  using AVCodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
  using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;

  static void save_with_ffmpeg(const AVFrame* frame, const std::string& filename);
  static void save_with_stb(const AVFrame* frame, const std::string& filename);

  static AVFrame* convert(const AVFrame* frame, const AVPixelFormat output_format);
};
