#pragma once
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
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
  VideoCompare(const int display_number,
               const Display::Mode display_mode,
               const bool verbose,
               const bool high_dpi_allowed,
               const bool use_10_bpc,
               const std::tuple<int, int> window_size,
               const double time_shift_ms,
               const std::string& left_file_name,
               const std::string& left_video_filters,
               const std::string& left_demuxer,
               const std::string& left_decoder,
               const std::string& left_hw_accel_spec,
               const std::string& right_file_name,
               const std::string& right_video_filters,
               const std::string& right_demuxer,
               const std::string& right_decoder,
               const std::string& right_hw_accel_spec,
               const bool disable_auto_filters);
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

  double time_shift_ms_;
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
  static const size_t QUEUE_SIZE;
  std::exception_ptr exception_{};
  volatile bool seeking_{false};
  volatile bool readyToSeek_[2][2];
};
