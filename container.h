#pragma once
#include <mutex>
#include <string>
extern "C" {
	#include "libavcodec/avcodec.h"
	#include "libavformat/avformat.h"
	#include "libavutil/avutil.h"
	#include "libswscale/swscale.h"
}

class Demuxer {
public:
	Demuxer(const std::string &file_name);
	~Demuxer();
	AVCodecContext* video_codec_context();
	int video_stream_index() const;
	AVRational time_base() const;
	bool operator()(AVPacket &packet);

private:
	AVFormatContext* format_context_{};
	int video_stream_index_{};
};

class VideoDecoder {
public:
	VideoDecoder(AVCodecContext* codec_context);
	~VideoDecoder();
	void operator()(AVFrame* frame, int &finished, AVPacket* packet);
	unsigned width() const;
	unsigned height() const;
	AVPixelFormat pixel_format() const;
	AVRational time_base() const;
private:
	AVCodecContext* codec_context_{};
};

class FormatConverter {
public:
	FormatConverter(
		size_t width, size_t height,
		AVPixelFormat input_pixel_format, AVPixelFormat output_pixel_format);
	void operator()(AVFrame* src, AVFrame* dst);
private:
	size_t width_;
	size_t height_;
	SwsContext* conversion_context_{};
};
