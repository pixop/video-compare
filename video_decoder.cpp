#include "video_decoder.h"
#include <iostream>
#include <string>
#include "ffmpeg.h"
#include "string_utils.h"

constexpr unsigned DEFAULT_SDR_NITS = 100;
constexpr unsigned DEFAULT_HDR_NITS = 500;

bool is_one_or_true(const char* str) {
  return strcmp(str, "1") == 0 || strcmp(str, "true") == 0 || strcmp(str, "t") == 0;
}

bool get_and_remove_bool_avdict_option(AVDictionary*& options, const char* key) {
  auto entry = av_dict_get(options, key, nullptr, 0);

  if (entry != nullptr) {
    auto value = is_one_or_true(entry->value);
    av_dict_set(&options, key, nullptr, 0);

    return value;
  }

  return false;
}

DynamicRange dynamic_range_from_trc_name(const std::string& trc_name) {
  if (trc_name == "smpte2084") {
    return DynamicRange::PQ;
  } else if (trc_name == "arib-std-b67") {
    return DynamicRange::HLG;
  }
  return DynamicRange::STANDARD;
}

DynamicRange dynamic_range_from_av_enum(const AVColorTransferCharacteristic color_trc) {
  switch (color_trc) {
    case AVCOL_TRC_SMPTE2084:
      return DynamicRange::PQ;
    case AVCOL_TRC_ARIB_STD_B67:
      return DynamicRange::HLG;
    default:
      return DynamicRange::STANDARD;
  }
}

VideoDecoder::VideoDecoder(const Side side,
                           const std::string& decoder_name,
                           const std::string& hw_accel_spec,
                           const AVCodecParameters* codec_parameters,
                           const unsigned peak_luminance_nits,
                           AVDictionary* hwaccel_options,
                           AVDictionary* decoder_options)
    : SideAware(side), hw_pixel_format_(AV_PIX_FMT_NONE), first_pts_(AV_NOPTS_VALUE), next_pts_(AV_NOPTS_VALUE), trust_decoded_pts_(false), peak_luminance_nits_(peak_luminance_nits) {
  ScopedLogSide scoped_log_side(side);

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
      auto device_name = hw_accel_spec.substr(colon_pos + 1);

      if (!device_name.empty()) {
        device = device_name.c_str();
      }
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

    if (av_hwdevice_ctx_create(&hw_device_ctx, hw_accel_type, device, hwaccel_options, 0) < 0) {
      throw ffmpeg::Error{"Failed to create a HW device context for " + hw_accel_name_};
    }

    ffmpeg::check_dict_is_empty(hwaccel_options, string_sprintf("HW acceleration %s", hw_accel_name_.c_str()));

    codec_context_->hw_device_ctx = hw_device_ctx;
  }

  // parse and remove any video-compare specific decoder options
  trust_decoded_pts_ = get_and_remove_bool_avdict_option(decoder_options, "trust_dec_pts");

  if (trust_decoded_pts_) {
    log_info("Trusting decoded PTS; extrapolation logic disabled.");
  }

  // open codec and check all options were consumed
  ffmpeg::check(avcodec_open2(codec_context_, codec_, &decoder_options));
  ffmpeg::check_dict_is_empty(decoder_options, string_sprintf("Decoder %s", codec_->name));
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

#if defined(AV_FRAME_FLAG_KEY)
  const bool is_key = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
#else
  const bool is_key = frame->key_frame != 0;
#endif

  const bool use_avframe_state = trust_decoded_pts_ || next_pts_ == AV_NOPTS_VALUE || is_key || frame->pts == first_pts_;
  const int64_t avframe_pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : (frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : 0);

  // use an increasing timestamp via pkt_duration between keyframes; otherwise, fall back to the best effort timestamp when PTS is not available
  frame->pts = (use_avframe_state || (next_pts_ + 1) == avframe_pts) ? avframe_pts : next_pts_;

  // ensure pkt_duration is always some sensible value
  if (ffmpeg::frame_duration(frame) == 0) {
    // estimate based on guessed frame rate
    ffmpeg::frame_duration(frame) = av_rescale_q(1, av_inv_q(demuxer->guess_frame_rate(frame)), demuxer->time_base());

    if (!use_avframe_state) {
      const int64_t avframe_delta_pts = avframe_pts - previous_pts_;

      // can avframe_delta_pts be relied on?
      if (abs(ffmpeg::frame_duration(frame) - avframe_delta_pts) <= (ffmpeg::frame_duration(frame) * 20 / 100)) {
        // use the delta between the current and previous PTS instead to reduce accumulated error
        ffmpeg::frame_duration(frame) = avframe_delta_pts;
      }
    }
  }

  first_pts_ = first_pts_ == AV_NOPTS_VALUE ? avframe_pts : first_pts_;
  previous_pts_ = avframe_pts;
  next_pts_ = frame->pts + ffmpeg::frame_duration(frame);

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

AVColorRange VideoDecoder::color_range() const {
  return codec_context_->color_range;
}

AVColorSpace VideoDecoder::color_space() const {
  return codec_context_->colorspace;
}

AVColorPrimaries VideoDecoder::color_primaries() const {
  return codec_context_->color_primaries;
}

AVColorTransferCharacteristic VideoDecoder::color_trc() const {
  return codec_context_->color_trc;
}

AVRational VideoDecoder::time_base() const {
  return codec_context_->time_base;
}

AVRational VideoDecoder::sample_aspect_ratio(const bool reduce) const {
  AVRational sar = codec_context_->sample_aspect_ratio;

  if (reduce) {
    av_reduce(&sar.num, &sar.den, sar.num, sar.den, 1024 * 1024);
  }

  return sar;
}

AVRational VideoDecoder::display_aspect_ratio() const {
  const AVRational sar = sample_aspect_ratio();

  AVRational dar;
  av_reduce(&dar.num, &dar.den, width() * static_cast<int64_t>(sar.num), height() * static_cast<int64_t>(sar.den), 1024 * 1024);

  return dar;
}

bool VideoDecoder::is_anamorphic() const {
  const AVRational sar = sample_aspect_ratio();

  return sar.num && (sar.num != sar.den);
}

int64_t VideoDecoder::next_pts() const {
  return next_pts_;
}

DynamicRange VideoDecoder::infer_dynamic_range(const std::string& trc_name) const {
  if (!trc_name.empty()) {
    return dynamic_range_from_trc_name(trc_name);
  }
  return dynamic_range_from_av_enum(color_trc());
}

unsigned VideoDecoder::safe_peak_luminance_nits(const DynamicRange dynamic_range) const {
  if (peak_luminance_nits_ != UNSET_PEAK_LUMINANCE) {
    return peak_luminance_nits_;
  } else {
    // resolve default peak luminance
    return (dynamic_range == DynamicRange::STANDARD) ? DEFAULT_SDR_NITS : DEFAULT_HDR_NITS;
  }
}
