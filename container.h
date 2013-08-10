#pragma once

#include "common.h"

class Container {
	private:
		AVFormatContext *format_context = nullptr;
		AVCodecContext *codec_context_video = nullptr;
		AVCodecContext *codec_context_audio = nullptr;
		AVCodec *codec_video = nullptr;
		AVCodec *codec_audio = nullptr;
		int video_stream = -1;
		int audio_stream = -1;

		void set_container(const char* file_name);
		void find_streams();
		void find_codecs();
	public:
		void init(const char* file_name);
		bool read_frame(AVPacket &packet);
		void decode_frame(AVFrame *frame, int &got_frame, AVPacket &packet);

		bool is_video();
		bool is_audio();

		int get_video_stream();
		int get_audio_stream();
		unsigned get_width();
		unsigned get_height();
		AVCodecContext* get_codec_context_video();
		AVPixelFormat get_pixel_format();
		SDL_AudioSpec get_audio_spec();
};
