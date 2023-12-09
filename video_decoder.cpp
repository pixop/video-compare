#include "video_decoder.h"
#include <iostream>
#include <string>
#include "ffmpeg.h"
#include "string_utils.h"

VideoDecoder::VideoDecoder(const std::string& decoder_name, const std::string& hw_accel_spec, AVCodecParameters* codec_parameters) : hw_pixel_format_(AV_PIX_FMT_NONE), next_pts_(AV_NOPTS_VALUE) {
  if (decoder_name.empty()) {
    codec_ = avcodec_find_decoder(codec_parameters->codec_id);
  } else {
    codec_ = avcodec_find_decoder_by_name(decoder_name.c_str());
  }

  if (codec_ == nullptr) {
    throw ffmpeg::Error{"Unsupported video codec"};
  }
  codec_context_ = avcodec_alloc_context3(codec_);
  if (codec_context_ == nullptr) {
    throw ffmpeg::Error{"Couldn't allocate video codec context"};
  }
  ffmpeg::check(avcodec_parameters_to_context(codec_context_, codec_parameters));

  // optionally set up hardware acceleration
  if (!hw_accel_spec.empty()) {
    const char* device = nullptr;

    const size_t colon_pos = hw_accel_spec.find(":");

    if (colon_pos == std::string::npos) {
      hw_accel_name_ = hw_accel_spec;
    } else {
      hw_accel_name_ = hw_accel_spec.substr(0, colon_pos);
      device = hw_accel_spec.substr(colon_pos + 1).c_str();
    }

    const AVHWDeviceType hw_accel_type = av_hwdevice_find_type_by_name(hw_accel_name_.c_str());

    if (hw_accel_type == AV_HWDEVICE_TYPE_NONE) {
      throw ffmpeg::Error{"Could not find HW acceleration: " + hw_accel_name_};
    }

    for (int i = 0;; i++) {
      const AVCodecHWConfig* config = avcodec_get_hw_config(codec_, i);

      if (!config) {
        throw ffmpeg::Error{string_sprintf("Decoder %s does not support HW device %s", codec_->name, hw_accel_name_.c_str())};
      }

      if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == hw_accel_type) {
        hw_pixel_format_ = config->pix_fmt;
        break;
      }
    }

    AVBufferRef* hw_device_ctx;

    if (av_hwdevice_ctx_create(&hw_device_ctx, hw_accel_type, device, nullptr, 0) < 0) {
      throw ffmpeg::Error{"Failed to create a HW device context for " + hw_accel_name_};
    }

    codec_context_->hw_device_ctx = hw_device_ctx;
  }

  ffmpeg::check(avcodec_open2(codec_context_, codec_, nullptr));
}

VideoDecoder::~VideoDecoder() {
  avcodec_free_context(&codec_context_);
}

const AVCodec* VideoDecoder::codec() const {
  return codec_;
}

AVCodecContext* VideoDecoder::codec_context() const {
  return codec_context_;
}

bool VideoDecoder::is_hw_accelerated() const {
  return codec_context_->hw_device_ctx != nullptr;
}

std::string VideoDecoder::hw_accel_name() const {
  return hw_accel_name_;
}

bool VideoDecoder::send(AVPacket* packet) {
  auto ret = avcodec_send_packet(codec_context_, packet);
  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
    return false;
  }
  ffmpeg::check(ret);
  return true;
}

bool VideoDecoder::receive(AVFrame* frame, Demuxer* demuxer) {
  auto ret = avcodec_receive_frame(codec_context_, frame);
  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
    return false;
  }
  ffmpeg::check(ret);

  const bool use_avframe_state = next_pts_ == AV_NOPTS_VALUE || frame->key_frame;
  const int64_t avframe_pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;

  // use an increasing timestamp via pkt_duration between keyframes; otherwise, fall back to the best effort timestamp when PTS is not available
  frame->pts = use_avframe_state ? avframe_pts : next_pts_;

  // ensure pkt_duration is always some sensible value
  if (frame->pkt_duration == 0) {
    // estimate based on guessed frame rate
    frame->pkt_duration = av_rescale_q(1, av_inv_q(demuxer->guess_frame_rate(frame)), demuxer->time_base());

    if (!use_avframe_state) {
      const int64_t avframe_delta_pts = avframe_pts - previous_pts_;

      // can avframe_delta_pts be relied on?
      if (abs(frame->pkt_duration - avframe_delta_pts) <= (frame->pkt_duration * 20 / 100)) {
        // use the delta between the current and previous PTS instead to reduce accumulated error
        frame->pkt_duration = avframe_delta_pts;
      }
    }
  }

  previous_pts_ = avframe_pts;
  next_pts_ = frame->pts + frame->pkt_duration;

  return true;
}

void VideoDecoder::flush() {
  avcodec_flush_buffers(codec_context_);
}

unsigned VideoDecoder::width() const {
  return codec_context_->width;
}

unsigned VideoDecoder::height() const {
  return codec_context_->height;
}

AVPixelFormat VideoDecoder::pixel_format() const {
  return codec_context_->pix_fmt;
}

AVPixelFormat VideoDecoder::hw_pixel_format() const {
  return hw_pixel_format_;
}

AVRational VideoDecoder::time_base() const {
  return codec_context_->time_base;
}

int64_t VideoDecoder::next_pts() const {
  return next_pts_;
}
