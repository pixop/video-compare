#include "demuxer.h"
#include <iostream>
#include "ffmpeg.h"

Demuxer::Demuxer(const std::string& demuxer_name, const std::string& file_name) {
  const AVInputFormat* input_format = nullptr;

  if (!demuxer_name.empty()) {
    input_format = av_find_input_format(demuxer_name.c_str());

    if (input_format == nullptr) {
      throw std::runtime_error(file_name + ": Demuxer '" + demuxer_name + "' not found");
    }
  }

  ffmpeg::check(file_name, avformat_open_input(&format_context_, file_name.c_str(), const_cast<AVInputFormat*>(input_format), nullptr));

  format_context_->probesize = 100000000;
  format_context_->max_analyze_duration = 100000000;

  ffmpeg::check(file_name, avformat_find_stream_info(format_context_, nullptr));
  video_stream_index_ = ffmpeg::check(file_name, av_find_best_stream(format_context_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0));
}

Demuxer::~Demuxer() {
  avformat_close_input(&format_context_);
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

int64_t Demuxer::start_time() const {
  return format_context_->start_time != AV_NOPTS_VALUE ? format_context_->start_time : 0;
}

int Demuxer::rotation() const {
  double theta = 0;

  uint8_t* displaymatrix = av_stream_get_side_data(format_context_->streams[video_stream_index_], AV_PKT_DATA_DISPLAYMATRIX, nullptr);

  if (displaymatrix != nullptr) {
    theta = -av_display_rotation_get(reinterpret_cast<int32_t*>(displaymatrix));
  }

  theta -= 360 * floor(theta / 360 + 0.9 / 360);

  return theta;
}

AVRational Demuxer::guess_frame_rate(AVFrame* frame) const {
  return av_guess_frame_rate(format_context_, format_context_->streams[video_stream_index_], frame);
}

bool Demuxer::operator()(AVPacket& packet) {
  return av_read_frame(format_context_, &packet) >= 0;
}

bool Demuxer::seek(const float position, const bool backward) {
  int64_t seek_target = static_cast<int64_t>(position * AV_TIME_BASE);

  return av_seek_frame(format_context_, -1, seek_target, backward ? AVSEEK_FLAG_BACKWARD : 0) >= 0;
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