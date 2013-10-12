#include "container.h"

#include <iostream>

using std::once_flag;
using std::string;
using std::runtime_error;
using std::unique_ptr;
using std::function;

once_flag Container::init_flag;

Container::Container(const string &file_name) :
	format_context(nullptr, [](AVFormatContext* c){ avformat_close_input(&c); avformat_free_context(c); }),
	codec_context_video(nullptr, [](AVCodecContext *c){ avcodec_close(c); }), 
	codec_context_audio(nullptr, [](AVCodecContext *c){ avcodec_close(c); }),
	conversion_context(nullptr, &sws_freeContext) {
	call_once(init_flag, [](){ av_register_all(); });
	parse_header(file_name);
	find_streams();
	find_codecs();
	setup_conversion_context();
}

void Container::parse_header(const string &file_name) {
	AVFormatContext* format = avformat_alloc_context();
	if (avformat_open_input(&format, file_name.c_str(), nullptr, nullptr) < 0) {
		throw runtime_error("Error opening input");
	}
	format_context.reset(format);
}

void Container::find_streams() {
	if (avformat_find_stream_info(format_context.get(), nullptr) < 0) {
	       throw runtime_error("Finding stream information");
	}

	for (size_t i = 0; i < format_context->nb_streams; ++i) {
		size_t codec_type = format_context->streams[i]->codec->codec_type;
		if (!is_video() && codec_type == AVMEDIA_TYPE_VIDEO) {
			video_stream.push_back(i);
		} else if (!is_audio() && codec_type == AVMEDIA_TYPE_AUDIO) {
			audio_stream.push_back(i);
		}
	}

	if (!is_video() && !is_audio()) {
		throw runtime_error("No audio or video stream");
	}
}

void Container::find_codecs() {
	if (is_video()) {
		codec_context_video.reset(format_context->streams[video_stream.front()]->codec);
		const auto codec_video = avcodec_find_decoder(codec_context_video.get()->codec_id);
		if (!codec_video) {
			throw runtime_error("Unsupported video codec");
		}
		if (avcodec_open2(codec_context_video.get(), codec_video, nullptr) < 0) {
			throw runtime_error("Opening video codec");
		}
	}
	if (is_audio()) {
		codec_context_audio.reset(format_context->streams[audio_stream.front()]->codec);
		const auto codec_audio = avcodec_find_decoder(codec_context_audio.get()->codec_id);
		if (!codec_audio) {
			throw runtime_error("Unsupported audio codec");
		}
		if (avcodec_open2(codec_context_audio.get(), codec_audio, nullptr) < 0) {
			throw runtime_error("Opening audio codec");
		}
	}
}

void Container::setup_conversion_context() {
	conversion_context.reset(
		sws_getContext(
			// Source
			get_width(), get_height(), get_pixel_format(), 
			// Destination
			get_width(), get_height(), PIX_FMT_YUV420P,
			// Filters
			SWS_BICUBIC, nullptr, nullptr, nullptr));
}

bool Container::read_frame(AVPacket &packet) {
	return av_read_frame(format_context.get(), &packet) >= 0;
}

void Container::decode_frame(unique_ptr<AVFrame, function<void(AVFrame*)>> &frame, int &got_frame, unique_ptr<AVPacket, function<void(AVPacket*)>> packet) {
	if (avcodec_decode_video2(codec_context_video.get(), frame.get(), &got_frame, packet.get()) < 0) {
		throw runtime_error("Decoding video");
	}
}

void Container::convert_frame(unique_ptr<AVFrame, function<void(AVFrame*)>> src, unique_ptr<AVFrame, function<void(AVFrame*)>> &dst) {
	sws_scale(conversion_context.get(),
		// Source
		src->data, src->linesize, 0, get_height(),
		// Destination
		dst->data, dst->linesize);	
}

void Container::decode_audio() {
	//while (packet.size > 0) {
	//	int frame_finished = 0;
	//	int length = avcodec_decode_audio4(codec_context, frame, &frame_finished, packet);

	//	if (length < 0) {
	//		// Error skip frame
	//		packet.size = 0;
	//		break;
	//	}

	//	if (frame_finished) {
	//		int data_size = av_samples_get_buffer_size(
	//			nullptr,
	//			codec_context->channels,
	//			frame->nb_samples,
	//			codec_context->sample_fmt,
	//			1);
	//		copy(frame->data[0][0], frame->data[0][data_size], audio_buffer);
	//	}

	//	packet.data += length;
	//	packet.size -= length;
	//}
}

bool Container::is_video() const {
       	return video_stream.size();
}

bool Container::is_audio() const {
	return audio_stream.size();
}

int Container::get_video_stream() const {
	return video_stream.front();
}

int Container::get_audio_stream() const {
	return audio_stream.front();
}

unsigned Container::get_width() const {
	return codec_context_video->width;
}

unsigned Container::get_height() const {
	return codec_context_video->height;
}

AVPixelFormat Container::get_pixel_format() const {
	return codec_context_video->pix_fmt;
}

AVRational Container::get_video_time_base() const {
	return codec_context_video->time_base;
}

AVRational Container::get_container_time_base() const {
	return format_context->streams[video_stream.front()]->time_base;
}
