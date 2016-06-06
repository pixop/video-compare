#include "format_converter.h"

FormatConverter::FormatConverter(
	size_t width, size_t height,
	AVPixelFormat input_pixel_format, AVPixelFormat output_pixel_format) :
	width_{width}, height_{height}, conversion_context_{sws_getContext(
		// Source
		width, height, input_pixel_format,
		// Destination
		width, height, output_pixel_format,
		// Filters
		SWS_BICUBIC, nullptr, nullptr, nullptr)} {
}

void FormatConverter::operator()(AVFrame* src, AVFrame* dst) {
	sws_scale(conversion_context_,
		// Source
		src->data, src->linesize, 0, height_,
		// Destination
		dst->data, dst->linesize);	
}
