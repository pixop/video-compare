#include "container.h"
#include <array>

namespace {
std::string error(const int error_code) {
	constexpr size_t size{64};
	std::array<char, size> buffer;
	av_make_error_string(buffer.data(), size, error_code);
	return "FFmpeg: " + std::string(buffer.data());
}

int check_ffmpeg(const int status) {
	if (status < 0) {
		throw std::runtime_error{error(status)};
	}
	return status;
}
}

Demuxer::Demuxer(const std::string &file_name) {
	av_register_all();
	check_ffmpeg(avformat_open_input(
		&format_context_, file_name.c_str(), nullptr, nullptr));
	check_ffmpeg(avformat_find_stream_info(
		format_context_, nullptr));
	video_stream_index_ = check_ffmpeg(av_find_best_stream(
		format_context_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0));
}
Demuxer::~Demuxer() {
	avformat_close_input(&format_context_);
}
AVCodecContext* Demuxer::video_codec_context() {
	return format_context_->streams[video_stream_index_]->codec;
}
int Demuxer::video_stream_index() const {
	return video_stream_index_;
}
AVRational Demuxer::time_base() const {
	return format_context_->streams[video_stream_index_]->time_base;
}
bool Demuxer::operator()(AVPacket &packet) {
	return av_read_frame(format_context_, &packet) >= 0;
}

VideoDecoder::VideoDecoder(AVCodecContext* codec_context) :
	codec_context_{codec_context} {
	avcodec_register_all();
	const auto codec_video =
		avcodec_find_decoder(codec_context_->codec_id);
	if (!codec_video) {
		throw std::runtime_error("Unsupported video codec");
	}
	check_ffmpeg(avcodec_open2(
		codec_context_, codec_video, nullptr));
}
VideoDecoder::~VideoDecoder() {
	avcodec_close(codec_context_);
}
void VideoDecoder::operator()(
	AVFrame* frame, int &finished, AVPacket* packet) {
	check_ffmpeg(avcodec_decode_video2(
		codec_context_, frame, &finished, packet));
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
AVRational VideoDecoder::time_base() const {
	return codec_context_->time_base;
}

FormatConverter::FormatConverter(
	size_t width, size_t height,
	AVPixelFormat input_pixel_format, AVPixelFormat output_pixel_format) :
	width_{width}, height_{height}, conversion_context_{sws_getContext(
		// Source
		width, height, input_pixel_format,
		// Destination
		width, height, output_pixel_format,
		// Filters
		SWS_BICUBIC, nullptr, nullptr, nullptr)} {
}
void FormatConverter::operator()(AVFrame* src, AVFrame* dst) {
	sws_scale(conversion_context_,
		// Source
		src->data, src->linesize, 0, height_,
		// Destination
		dst->data, dst->linesize);	
}
