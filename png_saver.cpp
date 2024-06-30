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
  const AVFrame* frame_to_save = frame;

  // Convert AV_PIX_FMT_RGB48LE to AV_PIX_FMT_RGB48BE
  AVFramePtr converted_frame(nullptr);

  if (frame->format == AV_PIX_FMT_RGB48LE) {
    converted_frame.reset(convert(frame, AV_PIX_FMT_RGB48BE));
    frame_to_save = converted_frame.get();
  }

  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
  if (!codec) {
    throw EncodingException("Codec not found");
  }
  AVCodecContextPtr codec_ctx(avcodec_alloc_context3(codec));
  if (!codec_ctx) {
    throw EncodingException("Could not allocate video codec context");
  }

  codec_ctx->width = frame_to_save->width;
  codec_ctx->height = frame_to_save->height;
  codec_ctx->pix_fmt = static_cast<enum AVPixelFormat>(frame_to_save->format);
  codec_ctx->time_base = {1, 25};

  if (avcodec_open2(codec_ctx.get(), codec, nullptr) < 0) {
    throw EncodingException("Could not open codec");
  }

  AVPacketPtr packet(av_packet_alloc());
  if (!packet) {
    throw EncodingException("Could not allocate packet");
  }

  if (avcodec_send_frame(codec_ctx.get(), frame_to_save) < 0) {
    throw EncodingException("Error sending a frame for encoding");
  }

  if (avcodec_receive_packet(codec_ctx.get(), packet.get()) < 0) {
    throw EncodingException("Error during encoding");
  }

  try {
    std::ofstream file(filename, std::ios::out | std::ios::binary);
    if (!file.is_open()) {
      throw IOException("Could not open file: " + filename);
    }
    file.write(reinterpret_cast<const char*>(packet->data), packet->size);
    file.close();
  } catch (const std::ios_base::failure& e) {
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
