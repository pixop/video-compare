#pragma once
#include <string>
extern "C" {
	#include "libavformat/avformat.h"
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
