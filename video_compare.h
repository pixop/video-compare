#pragma once
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "config.h"
#include "demuxer.h"
#include "display.h"
#include "format_converter.h"
#include "queue.h"
#include "timer.h"
#include "video_decoder.h"
#include "video_filterer.h"
extern "C" {
#include <libavcodec/avcodec.h>
}

using PacketQueue = Queue<std::unique_ptr<AVPacket, std::function<void(AVPacket*)>>>;
using FrameQueue = Queue<std::unique_ptr<AVFrame, std::function<void(AVFrame*)>>>;

class VideoCompare {
 public:
  VideoCompare(const VideoCompareConfig& config);
  void operator()();

 private:
  void thread_demultiplex_left();
  void thread_demultiplex_right();
  void demultiplex(int video_idx);

  void thread_decode_video_left();
  void thread_decode_video_right();
  void decode_video(int video_idx);
  bool process_packet(int video_idx, AVPacket* packet, AVFrame* frame_decoded, AVFrame* sw_frame_decoded = nullptr);
  bool filter_decoded_frame(int video_idx, AVFrame* frame_decoded);
  void video();

 private:
  static const size_t QUEUE_SIZE;

  Display::Loop auto_loop_mode_;
  const size_t frame_buffer_size_;
  const double time_shift_ms_;

  std::unique_ptr<Demuxer> demuxer_[2];
  std::unique_ptr<VideoDecoder> video_decoder_[2];
  std::unique_ptr<VideoFilterer> video_filterer_[2];

  size_t max_width_;
  size_t max_height_;
  double shortest_duration_;

  std::unique_ptr<FormatConverter> format_converter_[2];
  std::unique_ptr<Display> display_;
  std::unique_ptr<Timer> timer_;
  std::unique_ptr<PacketQueue> packet_queue_[2];
  std::unique_ptr<FrameQueue> frame_queue_[2];

  std::vector<std::thread> stages_;

  std::exception_ptr exception_{};

  volatile bool seeking_{false};
  volatile bool ready_to_seek_[2][2];
};
