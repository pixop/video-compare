#include "container.h"
#include <algorithm>
#include <iostream>

Container::Container(const std::string &file_name) {
	av_register_all();
	parse_header(file_name);
	find_streams();
	find_codecs();
	setup_conversion_context();
}

Container::~Container() {
	sws_freeContext(conversion_context_);
	avcodec_close(codec_context_audio_);
	avcodec_close(codec_context_video_);
	avformat_close_input(&format_context_);
	avformat_free_context(format_context_);
}

void Container::parse_header(const std::string &file_name) {
	format_context_ = avformat_alloc_context();
	if (avformat_open_input(&format_context_, file_name.c_str(),
	                        nullptr, nullptr) < 0) {
		throw std::runtime_error("Error opening input");
	}
}

void Container::find_streams() {
	if (avformat_find_stream_info(format_context_, nullptr) < 0) {
		throw std::runtime_error("Finding stream information");
	}

	for (size_t i = 0; i < format_context_->nb_streams; ++i) {
		size_t codec_type = format_context_->streams[i]->codec->codec_type;
		if (codec_type == AVMEDIA_TYPE_VIDEO) {
			video_stream_.push_back(i);
		} else if (codec_type == AVMEDIA_TYPE_AUDIO) {
			audio_stream_.push_back(i);
		}
	}

	if (!is_video() && !is_audio()) {
		throw std::runtime_error("No audio or video stream");
	}
}

void Container::find_codecs() {
	if (is_video()) {
		codec_context_video_ =
			format_context_->streams[video_stream_.front()]->codec;
		const auto codec_video =
			avcodec_find_decoder(codec_context_video_->codec_id);
		if (!codec_video) {
			throw std::runtime_error("Unsupported video codec");
		}
		if (avcodec_open2(codec_context_video_,
		                  codec_video, nullptr) < 0) {
			throw std::runtime_error("Opening video codec");
		}
	}
	if (is_audio()) {
		codec_context_audio_ =
			format_context_->streams[audio_stream_.front()]->codec;
		const auto codec_audio =
			avcodec_find_decoder(codec_context_audio_->codec_id);
		if (!codec_audio) {
			throw std::runtime_error("Unsupported audio codec");
		}
		if (avcodec_open2(codec_context_audio_,
		                  codec_audio, nullptr) < 0) {
			throw std::runtime_error("Opening audio codec");
		}
	}
}

void Container::setup_conversion_context() {
	conversion_context_ =
		sws_getContext(
			// Source
			get_width(), get_height(), get_pixel_format(), 
			// Destination
			get_width(), get_height(), AV_PIX_FMT_YUV420P,
			// Filters
			SWS_BICUBIC, nullptr, nullptr, nullptr);
}

bool Container::read_frame(AVPacket &packet) {
	return av_read_frame(format_context_, &packet) >= 0;
}

void Container::decode_frame(AVFrame* frame, int &finished, AVPacket* packet) {
	if (avcodec_decode_video2(codec_context_video_,
	                          frame, &finished, packet) < 0) {
		throw std::runtime_error("Decoding video");
	}
}

void Container::convert_frame(AVFrame* src, AVFrame* dst) {
	sws_scale(conversion_context_,
		// Source
		src->data, src->linesize, 0, get_height(),
		// Destination
		dst->data, dst->linesize);	
}

void Container::decode_audio(AVFrame* frame, int &finished, AVPacket* packet) {
	while (packet->size) {
		auto size = avcodec_decode_audio4(
			codec_context_audio_, frame, &finished, packet);
		if (size < 0) {
			throw std::runtime_error("Decoding audio.");
		}

		auto decoded = std::min(size, packet->size);
		packet->data += decoded;
		packet->size -= decoded;
	}
}

bool Container::is_video() const {
	return video_stream_.size();
}

bool Container::is_audio() const {
	return audio_stream_.size();
}

int Container::get_video_stream() const {
	return video_stream_.front();
}

int Container::get_audio_stream() const {
	return audio_stream_.front();
}

unsigned Container::get_width() const {
	return codec_context_video_->width;
}

unsigned Container::get_height() const {
	return codec_context_video_->height;
}

AVPixelFormat Container::get_pixel_format() const {
	return codec_context_video_->pix_fmt;
}

AVRational Container::get_video_time_base() const {
	return codec_context_video_->time_base;
}

AVRational Container::get_container_time_base() const {
	return format_context_->streams[video_stream_.front()]->time_base;
}
