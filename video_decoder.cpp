#include "video_decoder.h"
#include "ffmpeg.h"

VideoDecoder::VideoDecoder(AVCodecContext* codec_context) :
	codec_context_{codec_context} {
	avcodec_register_all();
	const auto codec_video =
		avcodec_find_decoder(codec_context_->codec_id);
	if (!codec_video) {
		throw ffmpeg::Error{"Unsupported video codec"};
	}
	ffmpeg::check(avcodec_open2(
		codec_context_, codec_video, nullptr));
}

VideoDecoder::~VideoDecoder() {
	avcodec_close(codec_context_);
}

bool VideoDecoder::send(AVPacket* packet) {
	auto ret = avcodec_send_packet(codec_context_, packet);
	if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
		return false;
	} else {
		ffmpeg::check(ret);
		return true;
	}
}

bool VideoDecoder::receive(AVFrame* frame) {
	auto ret = avcodec_receive_frame(codec_context_, frame);
	if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
		return false;
	} else {
		ffmpeg::check(ret);
		return true;
	}
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
