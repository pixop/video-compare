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
	void operator()(AVFrame* src, AVFrame* dst);
private:
	size_t width_;
	size_t height_;
	SwsContext* conversion_context_{};
};
