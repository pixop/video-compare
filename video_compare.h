#pragma once
#include <atomic>
#include <map>
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
#include "scope_manager.h"
#include "timer.h"
#include "video_decoder.h"
#include "video_filterer.h"
extern "C" {
#include <libavcodec/avcodec.h>
}

class ScopeWindow;

using AVPacketUniquePtr = std::unique_ptr<AVPacket, std::function<void(AVPacket*)>>;
using AVFrameSharedPtr = std::shared_ptr<AVFrame>;
using AVFrameUniquePtr = std::unique_ptr<AVFrame, std::function<void(AVFrame*)>>;

using PacketQueue = Queue<AVPacketUniquePtr>;
using DecodedFrameQueue = Queue<AVFrameSharedPtr>;
using FrameQueue = Queue<AVFrameUniquePtr>;

class ReadyToSeek {
 public:
  enum ProcessorThread { DEMULTIPLEXER, DECODER, FILTERER, CONVERTER, Count };

  bool get(const ProcessorThread i, const Side& j) const {
    auto it = ready_to_seek_[i].find(j);
    if (it != ready_to_seek_[i].end()) {
      return load(it->second);
    }
    // If not found, return false (not ready)
    return false;
  }

  void init(const ProcessorThread i, const Side& j) { ready_to_seek_[i][j].store(false, std::memory_order_relaxed); }

  void set(const ProcessorThread i, const Side& j) { store(ready_to_seek_[i][j], true); }

  void reset_all() {
    for (auto& thread_map : ready_to_seek_) {
      for (auto& pair : thread_map) {
        store(pair.second, false);
      }
    }
  }

  bool all_are_idle() const {
    for (const auto& thread_map : ready_to_seek_) {
      for (const auto& pair : thread_map) {
        if (!load(pair.second)) {
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
  std::array<std::map<Side, std::atomic_bool>, ProcessorThread::Count> ready_to_seek_;
};

class ExceptionHolder {
 public:
  void store_current_exception() {
    std::unique_lock<std::shared_timed_mutex> lock(mutex_);
    if (!exception_) {
      exception_ = std::current_exception();
    }
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

struct RightVideoInfo {
  std::string file_name;
  VideoMetadata metadata;
};

class VideoCompare {
 public:
  VideoCompare(const VideoCompareConfig& config);
  ~VideoCompare();
  void operator()();

 private:
  void thread_demultiplex(const Side& side);
  void demultiplex(const Side& side);

  void thread_decode_video(const Side& side);
  void decode_video(const Side& side);
  bool process_packet(const Side& side, AVPacket* packet);

  void thread_filter(const Side& side);
  void filter_video(const Side& side);
  void filter_decoded_frame(const Side& side, AVFrameSharedPtr frame_decoded);

  void thread_format_converter(const Side& side);
  void format_convert_video(const Side& side);

  bool keep_running() const;
  void quit_all_queues();

  void update_decoder_mode(const int right_time_shift);

  void dump_debug_info(const int frame_number, const int64_t effective_right_time_shift, const int average_refresh_time);

  void compare();

 private:
  const bool same_decoded_video_both_sides_;

  const Display::Loop auto_loop_mode_;
  const size_t frame_buffer_size_;
  const TimeShiftConfig time_shift_;
  const int64_t time_shift_offset_av_time_;

  std::map<Side, std::unique_ptr<Demuxer>> demuxers_;
  std::map<Side, std::unique_ptr<VideoDecoder>> video_decoders_;
  std::map<Side, std::unique_ptr<VideoFilterer>> video_filterers_;
  std::map<Side, std::unique_ptr<FormatConverter>> format_converters_;

  std::map<Side, std::unique_ptr<PacketQueue>> packet_queues_;
  std::map<Side, std::shared_ptr<DecodedFrameQueue>> decoded_frame_queues_;
  std::map<Side, std::unique_ptr<FrameQueue>> filtered_frame_queues_;
  std::map<Side, std::unique_ptr<FrameQueue>> converted_frame_queues_;

  size_t max_width_;
  size_t max_height_;
  const bool initial_fast_input_alignment_;
  double shortest_duration_;

  std::unique_ptr<Display> display_;
  std::unique_ptr<Timer> timer_;

  size_t active_right_index_{0};
  std::map<Side, RightVideoInfo> right_video_info_;

  std::unique_ptr<ScopeManager> scope_manager_;

  std::vector<std::thread> stages_;

  ExceptionHolder exception_holder_;

  std::atomic_bool seeking_{false};
  std::atomic_bool single_decoder_mode_{false};
  ReadyToSeek ready_to_seek_;
};
