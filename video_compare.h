#pragma once
#include <atomic>
#include <limits>
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
  enum class ProcessorThread { Demultiplexer, Decoder, Filterer, Converter, Count };

  bool get(const ProcessorThread i, const Side& j) const {
    auto it = ready_to_seek_[to_index(i)].find(j);
    if (it != ready_to_seek_[to_index(i)].end()) {
      return load(it->second);
    }
    // If not found, return false (not ready)
    return false;
  }

  void init(const ProcessorThread i, const Side& j) { ready_to_seek_[to_index(i)][j].store(false, std::memory_order_relaxed); }

  void set(const ProcessorThread i, const Side& j) { store(ready_to_seek_[to_index(i)][j], true); }

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
  static constexpr size_t to_index(const ProcessorThread thread) { return static_cast<size_t>(thread); }
  static constexpr size_t kProcessorThreadCount = static_cast<size_t>(ProcessorThread::Count);

  std::array<std::map<Side, std::atomic_bool>, kProcessorThreadCount> ready_to_seek_;
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

enum class MediaFrameCardinality { Unknown, SingleFrame, MultiFrame };

struct MediaFrameDetectionState {
  std::atomic<MediaFrameCardinality> cardinality{MediaFrameCardinality::Unknown};
  std::atomic_int decoded_count{0};
  std::atomic<int64_t> last_counted_pts{std::numeric_limits<int64_t>::min()};
};

class VideoCompare {
 public:
  VideoCompare(const VideoCompareConfig& config);
  ~VideoCompare();
  void operator()();

 private:
  void recreate_format_converter_for_side(const Side& side, const int sws_flags);
  void recreate_format_converters(const int sws_flags);

  void demultiplex(const Side& side);

  void decode_video(const Side& side);
  bool process_packet(const Side& side, AVPacket* packet);

  void filter_video(const Side& side);
  void filter_decoded_frame(const Side& side, AVFrameSharedPtr frame_decoded);

  void format_convert_video(const Side& side);

  bool keep_running() const;
  void quit_all_queues();

  void update_decoder_mode(const int right_time_shift);

  void note_decoded_frame(const Side& side, const int64_t pts);

  void refresh_side_filter_metadata(const Side& side);

  bool handle_pending_crop_request(const Side& active_right);
  std::vector<Side> consume_filter_changes();

  void dump_debug_info(const int frame_number, const int64_t effective_right_time_shift, const int average_refresh_time);

  void compare();

 private:
  class ScopeUpdateState {
   public:
    ScopeUpdateState() { reset(); }

    struct Sample {
      std::string left_frame_key;
      std::string right_frame_key;
      ScopeWindow::Roi roi;
      bool swapped;

      bool operator==(const Sample& other) const {
        const bool same_frame_keys = left_frame_key == other.left_frame_key && right_frame_key == other.right_frame_key;
        const bool same_roi = roi.x == other.roi.x && roi.y == other.roi.y && roi.w == other.roi.w && roi.h == other.roi.h;
        const bool same_swap = swapped == other.swapped;

        return same_frame_keys && same_roi && same_swap;
      }
    };

    static Sample capture(const AVFrame* left_frame, const AVFrame* right_frame, const ScopeWindow::Roi& roi, const bool swapped) {
      return Sample{
          get_frame_key(left_frame),
          get_frame_key(right_frame),
          roi,
          swapped,
      };
    }

    bool has_changed(const Sample& sample) const { return (!initialized_) || !(sample == state_); }

    void update(const Sample& sample) {
      state_ = sample;
      initialized_ = true;
    }

    void reset() { initialized_ = false; }

   private:
    Sample state_;
    bool initialized_{false};
  };

  const VideoCompareConfig& config_;
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

  std::map<Side, std::vector<SDL_Rect>> crop_history_;

  size_t max_width_;
  size_t max_height_;
  double shortest_duration_;

  std::unique_ptr<Display> display_;
  std::unique_ptr<Timer> timer_;

  size_t active_right_index_{0};
  std::map<Side, RightVideoInfo> right_video_info_;
  VideoMetadata left_video_metadata_;

  std::map<Side, MediaFrameDetectionState> media_frame_detection_states_;

  std::unique_ptr<ScopeManager> scope_manager_;
  ScopeUpdateState scope_update_state_;

  std::vector<std::thread> stages_;

  ExceptionHolder exception_holder_;

  std::atomic_bool seeking_{false};
  std::atomic_bool single_decoder_mode_{false};
  ReadyToSeek ready_to_seek_;
};
