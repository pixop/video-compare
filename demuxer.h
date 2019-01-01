#pragma once
#include <string>
extern "C" {
	#include "libavformat/avformat.h"
}

class Demuxer {
public:
	Demuxer(const std::string &file_name);
	~Demuxer();
	AVCodecParameters* video_codec_parameters();
	int video_stream_index() const;
	AVRational time_base() const;
	bool operator()(AVPacket &packet);
    bool seek(const float position, const bool backward);

private:
	AVFormatContext* format_context_{};
	int video_stream_index_{};
};
