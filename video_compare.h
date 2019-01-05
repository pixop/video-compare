#pragma once
#include "demuxer.h"
#include "display.h"
#include "format_converter.h"
#include "queue.h"
#include "timer.h"
#include "video_decoder.h"
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
extern "C" {
	#include <libavcodec/avcodec.h>
}

class VideoCompare {
public:
	VideoCompare(const bool high_dpi_allowed, const std::string &left_file_name, const std::string &right_file_name);
	void operator()();
private:
	void thread_demultiplex_left();
	void thread_demultiplex_right();
	void demultiplex(const int video_idx);

	void thread_decode_video_left();
	void thread_decode_video_right();
	void decode_video(const int video_idx);
	void video();
private:
	std::unique_ptr<Demuxer> demuxer_[2];
	std::unique_ptr<VideoDecoder> video_decoder_[2];
    size_t max_width_;
    size_t max_height_;
	std::unique_ptr<FormatConverter> format_converter_[2];
	std::unique_ptr<Display> display_;
	std::unique_ptr<Timer> timer_;
	std::unique_ptr<PacketQueue> packet_queue_[2];
	std::unique_ptr<FrameQueue> frame_queue_[2];
	std::vector<std::thread> stages_;
	static const size_t queue_size_;
	std::exception_ptr exception_{};
    volatile bool seeking_{false};
    volatile bool readyToSeek_[2][2];
};
