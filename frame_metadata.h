#pragma once

#include <cstdint>
#include <string>

struct AVFrame;

namespace FrameMetadata {

std::string require_key(const AVFrame* frame);
void set_key(AVFrame* frame, const std::string& key);

int get_filter_generation(const AVFrame* frame);
void set_filter_generation(AVFrame* frame, int generation);

std::string get_resolved_filters(const AVFrame* frame);
void set_resolved_filters(AVFrame* frame, const std::string& filters);

int get_original_width(const AVFrame* frame, int fallback);
int get_original_height(const AVFrame* frame, int fallback);
void set_original_dimensions(AVFrame* frame, int width, int height);

std::string make_frame_key(int64_t pts, int filter_generation);
std::string make_frame_key(const AVFrame* frame);

}  // namespace FrameMetadata
