#include "demuxer.h"
#include <iostream>
#include "ffmpeg.h"
#include "keyframe_index_probe.h"
#include "playback_keyframe.h"
#include "string_utils.h"

Demuxer::Demuxer(const Side& side, const std::string& demuxer_name, const std::string& file_name, AVDictionary* demuxer_options, const AVDictionary* decoder_options) : SideAware(side), demuxer_name_(demuxer_name), file_name_(file_name) {
  ScopedLogSide scoped_log_side(side);

  if (demuxer_options != nullptr) {
    av_dict_copy(&demuxer_options_copy_, demuxer_options, 0);
  }

  const AVInputFormat* input_format = nullptr;

  if (!demuxer_name.empty()) {
    input_format = av_find_input_format(demuxer_name.c_str());

    if (input_format == nullptr) {
      throw std::runtime_error(file_name + ": Demuxer '" + demuxer_name + "' not found");
    }
  }

  ffmpeg::check(file_name, avformat_open_input(&format_context_, file_name.c_str(), const_cast<AVInputFormat*>(input_format), &demuxer_options));
  ffmpeg::check_dict_is_empty(demuxer_options, string_sprintf("Demuxer %s", format_name().c_str()));

  // Try to find best stream first
  video_stream_index_ = av_find_best_stream(format_context_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

  const unsigned int nb_streams_before = format_context_->nb_streams;

  AVDictionary** opts_for_streams = (AVDictionary**)av_calloc(nb_streams_before, sizeof(AVDictionary*));

  // User-specified options are only used if we have a valid video stream index
  if (video_stream_index_ >= 0) {
    av_dict_copy(&opts_for_streams[video_stream_index_], decoder_options, 0);
  }

  // Achtung: avformat_find_stream_info() may modify the copied options
  ffmpeg::check(file_name, avformat_find_stream_info(format_context_, opts_for_streams));

  if (format_context_->nb_streams == 0) {
    throw std::runtime_error(file_name + ": No streams found in container");
  }

  if (video_stream_index_ < 0) {
    // Try manual search for video stream
    for (unsigned int i = 0; i < format_context_->nb_streams; i++) {
      const AVStream* stream = format_context_->streams[i];

      if (stream != nullptr && stream->codecpar != nullptr && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        video_stream_index_ = i;
        break;
      }
    }

    if (video_stream_index_ < 0) {
      throw std::runtime_error(file_name + ": No video stream found");
    }
  }

  for (unsigned int i = 0; i < nb_streams_before; i++) {
    av_dict_free(&opts_for_streams[i]);
  }

  av_freep(&opts_for_streams);

  keyframe_navigation_seekable_ = format_context_ != nullptr && format_context_->pb != nullptr && (format_context_->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;
}

Demuxer::~Demuxer() {
  close_probe_format_context();
  avformat_close_input(&format_context_);
  av_dict_free(&demuxer_options_copy_);
}

float Demuxer::video_start_time_seconds() const {
  return static_cast<float>(video_start_time()) * AV_TIME_TO_SEC;
}

AVCodecParameters* Demuxer::video_codec_parameters() {
  return format_context_->streams[video_stream_index_]->codecpar;
}

int Demuxer::video_stream_index() const {
  return video_stream_index_;
}

AVRational Demuxer::time_base() const {
  return format_context_->streams[video_stream_index_]->time_base;
}

int64_t Demuxer::duration() const {
  // use stream duration if available, otherwise use container duration if available, else 0
  const int64_t stream_duration = format_context_->streams[video_stream_index_]->duration;

  return stream_duration != AV_NOPTS_VALUE ? av_rescale_q(stream_duration, time_base(), AV_R_MICROSECONDS) : (format_context_->duration != AV_NOPTS_VALUE ? format_context_->duration : 0);
}

int64_t Demuxer::container_start_time() const {
  return format_context_->start_time != AV_NOPTS_VALUE ? format_context_->start_time : 0;
}

int64_t Demuxer::video_start_time() const {
  const AVStream* video_stream = format_context_->streams[video_stream_index_];

  if (video_stream->start_time != AV_NOPTS_VALUE) {
    return av_rescale_q(video_stream->start_time, video_stream->time_base, AV_R_MICROSECONDS);
  }

  return container_start_time();
}

int Demuxer::rotation() const {
  double theta = 0;
  AVStream* stream = format_context_->streams[video_stream_index_];

#if LIBAVFORMAT_VERSION_MAJOR >= 62
  const AVPacketSideData* side_data = av_packet_side_data_get(stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);

  // require 9 integers (3x3 matrix) + allow hypothetical padding
  if (side_data != nullptr && side_data->data != nullptr && side_data->size >= int(9 * sizeof(int32_t))) {
    theta = -av_display_rotation_get(reinterpret_cast<const int32_t*>(side_data->data));
  }
#else
  uint8_t* displaymatrix = av_stream_get_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, nullptr);

  if (displaymatrix != nullptr) {
    theta = -av_display_rotation_get(reinterpret_cast<int32_t*>(displaymatrix));
  }
#endif

  theta -= 360 * floor(theta / 360 + 0.9 / 360);

  return theta;
}

AVRational Demuxer::guess_frame_rate(AVFrame* frame) const {
  return av_guess_frame_rate(format_context_, format_context_->streams[video_stream_index_], frame);
}

AVRational Demuxer::sample_aspect_ratio(AVFrame* frame) const {
  AVStream* stream = format_context_->streams[video_stream_index_];

  if (stream == nullptr) {
    return AVRational{0, 1};
  }

  return av_guess_sample_aspect_ratio(format_context_, stream, frame);
}

bool Demuxer::operator()(AVPacket& packet) {
  return av_read_frame(format_context_, &packet) >= 0;
}

bool Demuxer::seek(const float position, const bool backward) {
  ScopedLogSide scoped_log_side(get_side());

  int64_t seek_target = static_cast<int64_t>(position * AV_TIME_BASE);

  return av_seek_frame(format_context_, -1, seek_target, backward ? AVSEEK_FLAG_BACKWARD : 0) >= 0;
}

bool Demuxer::seek_keyframe_owner(const float key_demuxer_seconds) {
  constexpr float kOwnerSeekBias = 1e-3F;
  return seek(key_demuxer_seconds + kOwnerSeekBias, true);
}

bool Demuxer::seek_keyframe_preroll(const float target_presentation_seconds) {
  if (!index_navigation_supported()) {
    return false;
  }

  constexpr float kReferenceEpsilon = 1e-4F;
  const playback_keyframe::AdjacentKeyframe prior = adjacent_keyframe(-1, target_presentation_seconds + kReferenceEpsilon);
  constexpr float kPrerollBias = 1e-3F;
  if (prior.status == playback_keyframe::AdjacentKeyframeStatus::AtBoundary) {
    // At the first keyframe there is no earlier index entry to preroll from.
    return seek(target_presentation_seconds + kPrerollBias, true);
  }
  if (prior.status != playback_keyframe::AdjacentKeyframeStatus::Ok) {
    return false;
  }

  return seek(prior.demuxer_position_seconds + kPrerollBias, true);
}

float Demuxer::probe_video_start_time_seconds() const {
  if (probe_format_context_ == nullptr || probe_video_stream_index_ < 0) {
    return 0.0F;
  }

  const AVStream* video_stream = probe_format_context_->streams[probe_video_stream_index_];
  if (video_stream != nullptr && video_stream->start_time != AV_NOPTS_VALUE) {
    return static_cast<float>(av_rescale_q(video_stream->start_time, video_stream->time_base, AV_R_MICROSECONDS)) * AV_TIME_TO_SEC;
  }

  const int64_t container_start = probe_format_context_->start_time != AV_NOPTS_VALUE ? probe_format_context_->start_time : 0;
  return static_cast<float>(container_start) * AV_TIME_TO_SEC;
}

bool Demuxer::ensure_keyframe_index_initialized() {
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(59, 0, 0)
  if (keyframe_index_init_attempted_) {
    return playback_keyframe::libavformat_index_api_available() && playback_keyframe::raw_index_has_keyframes(keyframe_index_snapshot_);
  }

  if (!playback_keyframe::libavformat_index_api_available()) {
    keyframe_index_init_attempted_ = true;
    keyframe_index_snapshot_ = {};
    return false;
  }

  if (!keyframe_navigation_seekable()) {
    keyframe_index_init_attempted_ = true;
    keyframe_index_snapshot_ = {};
    return false;
  }

  if (!ensure_probe_format_context()) {
    return false;
  }

  keyframe_index_init_attempted_ = true;
  // Some demuxers populate their keyframe index only after the first seek.
  const AVStream* stream = probe_format_context_->streams[probe_video_stream_index_];
  const int64_t start = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
  (void)av_seek_frame(probe_format_context_, probe_video_stream_index_, start, AVSEEK_FLAG_BACKWARD);
  keyframe_index_snapshot_ = playback_keyframe::capture_keyframe_index_snapshot(probe_format_context_, probe_video_stream_index_, probe_video_start_time_seconds());
  return playback_keyframe::raw_index_has_keyframes(keyframe_index_snapshot_);
#else
  keyframe_index_init_attempted_ = true;
  keyframe_index_snapshot_ = {};
  return false;
#endif
}

bool Demuxer::index_navigation_supported() {
  return ensure_keyframe_index_initialized();
}

bool Demuxer::probe_context_matches_live() const {
  if (probe_format_context_ == nullptr || format_context_ == nullptr || probe_video_stream_index_ < 0) {
    return false;
  }

  const AVStream* live = format_context_->streams[video_stream_index_];
  const AVStream* probe = probe_format_context_->streams[probe_video_stream_index_];
  if (live == nullptr || probe == nullptr || live->codecpar == nullptr || probe->codecpar == nullptr) {
    return false;
  }

  if (live->codecpar->codec_id != probe->codecpar->codec_id) {
    return false;
  }

  if (live->time_base.num != probe->time_base.num || live->time_base.den != probe->time_base.den) {
    return false;
  }

  const int64_t live_duration = live->duration;
  const int64_t probe_duration = probe->duration;
  if (live_duration != AV_NOPTS_VALUE && probe_duration != AV_NOPTS_VALUE && live_duration != probe_duration) {
    return false;
  }

  const int64_t live_start = live->start_time;
  const int64_t probe_start = probe->start_time;
  if (live_start != AV_NOPTS_VALUE && probe_start != AV_NOPTS_VALUE && live_start != probe_start) {
    return false;
  }

  return true;
}

bool Demuxer::ensure_probe_format_context() {
  if (probe_context_open_ && probe_format_context_ != nullptr && probe_context_matches_live()) {
    return true;
  }

  close_probe_format_context();

  const AVInputFormat* input_format = nullptr;
  if (!demuxer_name_.empty()) {
    input_format = av_find_input_format(demuxer_name_.c_str());
  }

  AVDictionary* probe_options = nullptr;
  if (demuxer_options_copy_ != nullptr) {
    av_dict_copy(&probe_options, demuxer_options_copy_, 0);
  }

  if (avformat_open_input(&probe_format_context_, file_name_.c_str(), const_cast<AVInputFormat*>(input_format), &probe_options) < 0) {
    av_dict_free(&probe_options);
    return false;
  }
  av_dict_free(&probe_options);

  if (avformat_find_stream_info(probe_format_context_, nullptr) < 0) {
    close_probe_format_context();
    return false;
  }

  if (static_cast<unsigned int>(video_stream_index_) >= probe_format_context_->nb_streams) {
    close_probe_format_context();
    return false;
  }
  probe_video_stream_index_ = video_stream_index_;

  probe_context_open_ = probe_context_matches_live();
  if (!probe_context_open_) {
    close_probe_format_context();
  }

  return probe_context_open_;
}

void Demuxer::close_probe_format_context() {
  if (probe_format_context_ != nullptr) {
    avformat_close_input(&probe_format_context_);
  }
  probe_video_stream_index_ = -1;
  probe_context_open_ = false;
}

playback_keyframe::AdjacentKeyframe Demuxer::adjacent_keyframe(const int direction, const float reference_demuxer_seconds) {
  playback_keyframe::AdjacentKeyframe result;

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(59, 0, 0)
  if (!index_navigation_supported() || direction == 0) {
    return result;
  }

  if (!ensure_probe_format_context()) {
    return result;
  }

  if (!playback_keyframe::prepare_adjacent_keyframe_probe(probe_format_context_, probe_video_stream_index_, keyframe_index_snapshot_, direction, reference_demuxer_seconds)) {
    return result;
  }

  return playback_keyframe::find_adjacent_keyframe(keyframe_index_snapshot_, direction, reference_demuxer_seconds);
#else
  (void)direction;
  (void)reference_demuxer_seconds;
  return result;
#endif
}

std::string Demuxer::format_name() {
  return format_context_->iformat->name;
}

int64_t Demuxer::file_size() {
  return avio_size(format_context_->pb);
}

int64_t Demuxer::bit_rate() {
  // use stream bit rate if available, otherwise use container bit rate
  const int64_t stream_bit_rate = format_context_->streams[video_stream_index_]->codecpar->bit_rate;

  return stream_bit_rate > 0 ? stream_bit_rate : format_context_->bit_rate;
}