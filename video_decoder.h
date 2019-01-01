#pragma once
extern "C" {
	#include "libavcodec/avcodec.h"
}

class VideoDecoder {
public:
	VideoDecoder(AVCodecParameters* codec_parameters);
	~VideoDecoder();
	bool send(AVPacket* packet);
	bool receive(AVFrame* frame);
    void flush();
	unsigned width() const;
	unsigned height() const;
	AVPixelFormat pixel_format() const;
	AVRational time_base() const;
private:
	AVCodecContext* codec_context_{};
};
