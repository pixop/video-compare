#pragma once
#include <atomic>
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

enum Side {
  LEFT = 0,
  RIGHT = 1
};

class ReadyToSeek {
public:
  enum ProcessorThread {
    DEMULTIPLEXER = 0,
    DECODER = 1
  };

  ReadyToSeek() {
    reset();
  }

  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    ready_to_seek_[DEMULTIPLEXER][LEFT] = false;
    ready_to_seek_[DEMULTIPLEXER][RIGHT] = false;
    ready_to_seek_[DECODER][LEFT] = false;
    ready_to_seek_[DECODER][RIGHT] = false;
  }

  void set(const ProcessorThread i, const Side j) {
    std::lock_guard<std::mutex> lock(mutex_);
    ready_to_seek_[i][j] = true;
  }

  bool get(const ProcessorThread i, const Side j) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_to_seek_[i][j];
  }

  bool all_are_empty() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_to_seek_[DEMULTIPLEXER][LEFT] && ready_to_seek_[DEMULTIPLEXER][RIGHT] && 
           ready_to_seek_[DECODER][LEFT] && ready_to_seek_[DECODER][RIGHT];
  }

private:
  std::mutex mutex_;
  std::array<std::array<bool, sizeof(ProcessorThread)>, sizeof(Side)> ready_to_seek_;
};

class VideoCompare {
 public:
  VideoCompare(const VideoCompareConfig& config);
  void operator()();

 private:
  void thread_demultiplex_left();
  void thread_demultiplex_right();
  void demultiplex(const Side side);

  void thread_decode_video_left();
  void thread_decode_video_right();
  void decode_video(const Side side);
  bool process_packet(const Side side, AVPacket* packet, AVFrame* frame_decoded, AVFrame* sw_frame_decoded = nullptr);
  bool filter_decoded_frame(const Side side, AVFrame* frame_decoded);
  void video();

 private:
  static const size_t QUEUE_SIZE;

  Display::Loop auto_loop_mode_;
  const size_t frame_buffer_size_;
  const double time_shift_ms_;

  std::unique_ptr<Demuxer> demuxer_[sizeof(Side)];
  std::unique_ptr<VideoDecoder> video_decoder_[sizeof(Side)];
  std::unique_ptr<VideoFilterer> video_filterer_[sizeof(Side)];

  size_t max_width_;
  size_t max_height_;
  double shortest_duration_;

  std::unique_ptr<FormatConverter> format_converter_[sizeof(Side)];
  std::unique_ptr<Display> display_;
  std::unique_ptr<Timer> timer_;
  std::unique_ptr<PacketQueue> packet_queue_[sizeof(Side)];
  std::unique_ptr<FrameQueue> frame_queue_[sizeof(Side)];

  std::vector<std::thread> stages_;

  std::exception_ptr exception_{};

  std::atomic_bool seeking_{false};
  ReadyToSeek ready_to_seek_;
};
