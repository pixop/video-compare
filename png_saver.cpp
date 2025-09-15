#include "png_saver.h"
#include <fstream>
extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

void PngSaver::save(const AVFrame* frame, const std::string& filename) {
  try {
    // use FFmpeg PNG encoder if available in this libavcodec build, as it is faster and generally produces smaller files than stb
    save_with_ffmpeg(frame, filename);
  } catch (const PngSaver::EncodingException& e) {
    // fall back on stb implementation
    save_with_stb(frame, filename);
  }
}

void PngSaver::save_with_ffmpeg(const AVFrame* frame, const std::string& filename) {
  // Step 1: Create a scaling context (SwsContext) for YUV → RGB conversion
  auto sws_ctx = std::unique_ptr<SwsContext, void(*)(SwsContext*)>(
      sws_getContext(
          frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
          frame->width, frame->height, AV_PIX_FMT_RGB24,
          SWS_BILINEAR, nullptr, nullptr, nullptr
      ),
      [](SwsContext* ctx) { if (ctx) sws_freeContext(ctx); }
  );

  if (!sws_ctx) {
    throw EncodingException("Cannot create scaling context");
  }

  // Step 2: Allocate and initialize the target RGB frame
  auto rgb_frame = std::unique_ptr<AVFrame, void(*)(AVFrame*)>(
      av_frame_alloc(),
      [](AVFrame* frame) { if (frame) av_frame_free(&frame); }
  );

  if (!rgb_frame) {
    throw EncodingException("Could not allocate RGB frame");
  }

  rgb_frame->width = frame->width;
  rgb_frame->height = frame->height;
  rgb_frame->format = AV_PIX_FMT_RGB24;

  if (av_frame_get_buffer(rgb_frame.get(), 32) < 0) {
    throw EncodingException("Could not allocate frame data");
  }

  // Step 3: Perform color space and range conversion (automatically handles TV → PC)
  int ret = sws_scale(sws_ctx.get(),
                      frame->data, frame->linesize, 0, frame->height,
                      rgb_frame->data, rgb_frame->linesize);
  if (ret < 0) {
    throw EncodingException("Error during scaling");
  }

  // Step 4: Initialize PNG encoder context
  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
  if (!codec) {
    throw EncodingException("PNG codec not found");
  }

  auto codec_ctx = std::unique_ptr<AVCodecContext, void(*)(AVCodecContext*)>(
      avcodec_alloc_context3(codec),
      [](AVCodecContext* ctx) { if (ctx) avcodec_free_context(&ctx); }
  );

  if (!codec_ctx) {
    throw EncodingException("Could not allocate codec context");
  }

  codec_ctx->width = rgb_frame->width;
  codec_ctx->height = rgb_frame->height;
  codec_ctx->pix_fmt = AV_PIX_FMT_RGB24;
  codec_ctx->time_base = {1, 1};

  if (avcodec_open2(codec_ctx.get(), codec, nullptr) < 0) {
    throw EncodingException("Could not open codec");
  }

  // Step 5: Allocate and encode frame
  auto packet = std::unique_ptr<AVPacket, void(*)(AVPacket*)>(
      av_packet_alloc(),
      [](AVPacket* pkt) { if (pkt) av_packet_free(&pkt); }
  );

  if (!packet) {
    throw EncodingException("Could not allocate packet");
  }

  if (avcodec_send_frame(codec_ctx.get(), rgb_frame.get()) < 0) {
    throw EncodingException("Error sending a frame for encoding");
  }

  if (avcodec_receive_packet(codec_ctx.get(), packet.get()) < 0) {
    throw EncodingException("Error during encoding");
  }

  // Step 6: Write to file
  try {
    std::ofstream file(filename, std::ios::out | std::ios::binary);
    if (!file.is_open()) {
      throw IOException("Could not open file: " + filename);
    }
    file.write(reinterpret_cast<const char*>(packet->data), packet->size);
    file.close();
  } catch (const std::exception& e) {
    throw IOException("IO error while writing file " + filename + ": " + e.what());
  }
}

void PngSaver::save_with_stb(const AVFrame* frame, const std::string& filename) {
  if (frame->format == AV_PIX_FMT_RGB24) {
    if (stbi_write_png(filename.c_str(), frame->width, frame->height, 3, frame->data[0], frame->linesize[0]) == 0) {
      throw IOException("Error while writing PNG via stb: " + filename);
    }
  } else if (frame->format == AV_PIX_FMT_RGB48LE) {
    if (stbi_write_png_16(filename.c_str(), frame->width, frame->height, 3, frame->data[0], frame->linesize[0]) == 0) {
      throw IOException("Error while writing PNG via stb: " + filename);
    }
  } else {
    throw EncodingException("Pixel format not supported by stb");
  }
}

AVFrame* PngSaver::convert(const AVFrame* frame, const AVPixelFormat output_format) {
  struct SwsContext* sws_ctx = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, output_format, SWS_BILINEAR, nullptr, nullptr, nullptr);

  if (!sws_ctx) {
    throw EncodingException("Could not initialize the conversion context");
  }

  AVFrame* converted_frame = av_frame_alloc();
  if (!converted_frame) {
    sws_freeContext(sws_ctx);
    throw EncodingException("Could not allocate converted frame");
  }

  converted_frame->format = output_format;
  converted_frame->width = frame->width;
  converted_frame->height = frame->height;
  av_image_alloc(converted_frame->data, converted_frame->linesize, converted_frame->width, converted_frame->height, output_format, 32);

  sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, converted_frame->data, converted_frame->linesize);

  sws_freeContext(sws_ctx);
  return converted_frame;
}
