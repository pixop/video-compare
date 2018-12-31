#pragma once
extern "C" {
	#include "libavformat/avformat.h"
	#include "libswscale/swscale.h"
}

class FormatConverter {
public:
	FormatConverter(
		size_t width, size_t height,
		AVPixelFormat input_pixel_format, AVPixelFormat output_pixel_format);
	AVPixelFormat output_pixel_format() const;
	void operator()(AVFrame* src, AVFrame* dst);
private:
	size_t width_;
	size_t height_;
	AVPixelFormat output_pixel_format_;
	SwsContext* conversion_context_{};
};
