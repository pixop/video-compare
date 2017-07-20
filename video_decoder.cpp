#include "video_decoder.h"
#include "ffmpeg.h"

VideoDecoder::VideoDecoder(AVCodecParameters* codec_parameters) {
	avcodec_register_all();
	const auto codec = avcodec_find_decoder(codec_parameters->codec_id);
	if (!codec) {
		throw ffmpeg::Error{"Unsupported video codec"};
	}
	codec_context_ = avcodec_alloc_context3(codec);
	if (!codec_context_) {
		throw ffmpeg::Error{"Couldn't allocate video codec context"};
	}
	ffmpeg::check(avcodec_parameters_to_context(
		codec_context_, codec_parameters));
	ffmpeg::check(avcodec_open2(codec_context_, codec, nullptr));
}

VideoDecoder::~VideoDecoder() {
	avcodec_free_context(&codec_context_);
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
