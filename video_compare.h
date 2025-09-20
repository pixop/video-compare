#pragma once
#include <atomic>
#include <memory>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "config.h"
#include "core_types.h"
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

using AVPacketUniquePtr = std::unique_ptr<AVPacket, std::function<void(AVPacket*)>>;
using AVFrameSharedPtr = std::shared_ptr<AVFrame>;
using AVFrameUniquePtr = std::unique_ptr<AVFrame, std::function<void(AVFrame*)>>;

using PacketQueue = Queue<AVPacketUniquePtr>;
using DecodedFrameQueue = Queue<AVFrameSharedPtr>;
using FrameQueue = Queue<AVFrameUniquePtr>;

class ReadyToSeek {
 public:
  enum ProcessorThread { DEMULTIPLEXER, DECODER, FILTERER, CONVERTER, Count };

  ReadyToSeek() { reset(); }

  void reset() {
    for (auto& thread_array : ready_to_seek_) {
      for (auto& flag : thread_array) {
        store(flag, false);
      }
    }
  }

  bool get(const ProcessorThread i, const Side j) const { return load(ready_to_seek_[i][j]); }

  void set(const ProcessorThread i, const Side j) { store(ready_to_seek_[i][j], true); }

  bool all_are_idle() const {
    for (const auto& thread_array : ready_to_seek_) {
      for (const auto& flag : thread_array) {
        if (!load(flag)) {
          return false;
        }
      }
    }

    return true;
  }

 private:
  static inline bool load(const std::atomic_bool& atomic_flag) { return atomic_flag.load(std::memory_order_relaxed); }

  static inline void store(std::atomic_bool& atomic_flag, const bool value) { atomic_flag.store(value, std::memory_order_relaxed); }

 private:
  std::array<std::array<std::atomic_bool, Side::Count>, ProcessorThread::Count> ready_to_seek_;
};

class ExceptionHolder {
 public:
  void store_current_exception() {
    std::unique_lock<std::shared_timed_mutex> lock(mutex_);
    exception_ = std::current_exception();
  }

  bool has_exception() const {
    std::shared_lock<std::shared_timed_mutex> lock(mutex_);
    return exception_ != nullptr;
  }

  void rethrow_stored_exception() const {
    std::shared_lock<std::shared_timed_mutex> lock(mutex_);
    if (exception_) {
      std::rethrow_exception(exception_);
    }
  }

 private:
  mutable std::shared_timed_mutex mutex_;
  std::exception_ptr exception_{nullptr};
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
  bool process_packet(const Side side, AVPacket* packet);

  void thread_filter_left();
  void thread_filter_right();
  void filter_video(const Side side);
  void filter_decoded_frame(const Side side, AVFrameSharedPtr frame_decoded);

  void thread_format_converter_left();
  void thread_format_converter_right();
  void format_convert_video(const Side side);

  bool keep_running() const;
  void quit_queues(const Side side);

  void update_decoder_mode(const int right_time_shift);

  void dump_debug_info(const int frame_number, const int right_time_shift, const int average_refresh_time);

  void compare();

 private:
  const bool same_decoded_video_both_sides_;

  const Display::Loop auto_loop_mode_;
  const size_t frame_buffer_size_;
  const TimeShiftConfig time_shift_;
  const int64_t time_shift_offset_av_time_;

  const std::array<std::unique_ptr<Demuxer>, Side::Count> demuxers_;
  const std::array<std::unique_ptr<VideoDecoder>, Side::Count> video_decoders_;
  const std::array<std::unique_ptr<VideoFilterer>, Side::Count> video_filterers_;

  const size_t max_width_;
  const size_t max_height_;
  const bool initial_fast_input_alignment_;
  const double shortest_duration_;

  const std::array<std::unique_ptr<FormatConverter>, Side::Count> format_converters_;
  const std::unique_ptr<Display> display_;
  const std::unique_ptr<Timer> timer_;
  const std::array<std::unique_ptr<PacketQueue>, Side::Count> packet_queues_;
  const std::array<std::shared_ptr<DecodedFrameQueue>, Side::Count> decoded_frame_queues_;
  const std::array<std::unique_ptr<FrameQueue>, Side::Count> filtered_frame_queues_;
  const std::array<std::unique_ptr<FrameQueue>, Side::Count> converted_frame_queues_;

  std::vector<std::thread> stages_;

  ExceptionHolder exception_holder_;

  std::atomic_bool seeking_{false};
  std::atomic_bool single_decoder_mode_{false};
  ReadyToSeek ready_to_seek_;
};
