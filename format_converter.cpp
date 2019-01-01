#include "format_converter.h"
#include <iostream>

FormatConverter::FormatConverter(
	size_t src_width, size_t src_height,
	size_t dest_width, size_t dest_height,
	AVPixelFormat input_pixel_format, AVPixelFormat output_pixel_format) :
	src_width_{src_width}, src_height_{src_height}, 
	dest_width_{dest_width}, dest_height_{dest_width}, 
	output_pixel_format_{output_pixel_format}, conversion_context_{sws_getContext(
		// Source
		src_width, src_height, input_pixel_format,
		// Destination
		dest_width, dest_height, output_pixel_format,
		// Filters
		SWS_BICUBIC, nullptr, nullptr, nullptr)} {
}

size_t FormatConverter::dest_width() const {
	return dest_width_;
}

size_t FormatConverter::dest_height() const {
	return dest_height_;
}

AVPixelFormat FormatConverter::output_pixel_format() const {
	return output_pixel_format_;
}

void FormatConverter::operator()(AVFrame* src, AVFrame* dst) {
	sws_scale(conversion_context_,
		// Source
		src->data, src->linesize, 0, src_height_,
		// Destination
		dst->data, dst->linesize);	
}
