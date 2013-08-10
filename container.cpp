#include "container.h"

using namespace std;

void Container::set_container(const char* file_name) {
	// Parse header
	if (avformat_open_input(&format_context, file_name, nullptr, nullptr) < 0) {
		throw runtime_error("Opening input");
	}
}
void Container::find_streams() {
	if (avformat_find_stream_info(format_context, nullptr) < 0) {
	       throw runtime_error("Finding stream information");
	}

	// Store first audio/video stream
	for (size_t i = 0; i < format_context->nb_streams; ++i) {
		if (is_video() && is_audio()) { 
			return;
		}
		size_t codec_type = format_context->streams[i]->codec->codec_type;
		if (!is_video() && codec_type == AVMEDIA_TYPE_VIDEO) {
			video_stream = i;
		} else if (!is_audio() && codec_type == AVMEDIA_TYPE_AUDIO) {
			audio_stream = i;
		}
	}
	if (!is_video() && !is_audio()) {
		throw runtime_error("No audio or video stream");
	}
}
void Container::find_codecs() {
	if (is_video()) {
		codec_context_video = format_context->streams[video_stream]->codec;
		codec_video = avcodec_find_decoder(codec_context_video->codec_id);
		if (!codec_video)
			throw runtime_error("Unsupported video codec");
		if (avcodec_open2(codec_context_video, codec_video, nullptr) < 0)
			throw runtime_error("Opening video codec");
	}
	if (is_audio()) {
		codec_context_audio = format_context->streams[audio_stream]->codec;
		codec_audio = avcodec_find_decoder(codec_context_audio->codec_id);
		if (!codec_audio)
			throw runtime_error("Unsupported audio codec");
		if (avcodec_open2(codec_context_audio, codec_audio, nullptr) < 0)
			throw runtime_error("Opening audio codec");
	}
}

void Container::init(const char* file_name) {
	set_container(file_name);
	find_streams();
	find_codecs();
}
bool Container::read_frame(AVPacket &packet) {
	return av_read_frame(format_context, &packet) >= 0;
}
void Container::decode_frame(AVFrame *frame, int &got_frame, AVPacket &packet) {
	clog << "in" << endl;
	if (avcodec_decode_video2(codec_context_video, frame, &got_frame, &packet) < 0) {
		cerr << "out" << endl;
		throw runtime_error("Decoding video");
	}
	clog << "out" << endl;
}

bool Container::is_video() { return video_stream != -1; }
bool Container::is_audio() { return audio_stream != -1; }

int Container::get_video_stream() { return video_stream; }
int Container::get_audio_stream() { return audio_stream; }
unsigned Container::get_width() { return codec_context_video->width; }
unsigned Container::get_height() { return codec_context_video->height; }
AVCodecContext* Container::get_codec_context_video() { return codec_context_video; }
AVPixelFormat Container::get_pixel_format() { return codec_context_video->pix_fmt; }
SDL_AudioSpec Container::get_audio_spec() {
	if (!is_audio()) {
		return (SDL_AudioSpec){0,0,0,0,0,0,0,nullptr};
	}
	SDL_AudioSpec spec;
	spec.freq = codec_context_audio->sample_rate;
	spec.format = AUDIO_S16SYS;
	spec.channels = codec_context_audio->channels;
	spec.silence = 0;
	spec.samples = 1024;
	spec.callback = nullptr;//audio_callback;
	spec.userdata = codec_context_audio;
	return spec;
}
