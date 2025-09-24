#include "video_compare.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <thread>
#include "ffmpeg.h"
#include "side_aware_logger.h"
#include "sorted_flat_deque.h"
#include "string_utils.h"
extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
}

static constexpr size_t QUEUE_SIZE = 5;

static constexpr uint32_t SLEEP_PERIOD_MS = 10;

static constexpr uint32_t ONE_SECOND_US = 1000 * 1000;
static constexpr uint32_t RESYNC_UPDATE_RATE_US = ONE_SECOND_US / 10;
static constexpr uint32_t NOMINAL_FPS_UPDATE_RATE_US = 1 * ONE_SECOND_US;

static auto avpacket_deleter = [](AVPacket* packet) {
  av_packet_unref(packet);
  delete packet;
};

static auto avframe_deleter = [](AVFrame* frame) { av_frame_free(&frame); };

static auto avframe_and_data_deleter = [](AVFrame* frame) {
  av_freep(&frame->data[0]);
  avframe_deleter(frame);
};

static inline bool is_behind(int64_t frame1_pts, int64_t frame2_pts, int64_t delta_pts) {
  const float t1 = static_cast<float>(frame1_pts) * AV_TIME_TO_SEC;
  const float t2 = static_cast<float>(frame2_pts) * AV_TIME_TO_SEC;
  const float delta_s = static_cast<float>(delta_pts) * AV_TIME_TO_SEC - 1e-5F;

  const float diff = t1 - t2;
  const float tolerance = std::max(delta_s, 1.0F / 480.0F);

  return diff < -tolerance;
}

static inline int64_t compute_min_delta(const int64_t delta_left_pts, const int64_t delta_right_pts) {
  return std::min(delta_left_pts, delta_right_pts) * 8 / 10;
};

static inline bool is_in_sync(const int64_t left_pts, const int64_t right_pts, const int64_t delta_left_pts, const int64_t delta_right_pts) {
  const int64_t min_delta = compute_min_delta(delta_left_pts, delta_right_pts);

  return !is_behind(left_pts, right_pts, min_delta) && !is_behind(right_pts, left_pts, min_delta);
};

static inline int64_t compute_frame_delay(const int64_t left_pts, const int64_t right_pts) {
  return std::max(left_pts, right_pts);
}

static inline int64_t time_ms_to_av_time(const double time_ms) {
  return time_ms * MILLISEC_TO_AV_TIME;
}

static inline int64_t calculate_dynamic_time_shift(const AVRational& multiplier, const int64_t original_pts, const bool inverse) {
  // Calculate the time shift as the difference between original and scaled PTS
  const int64_t time_shift =
      inverse ? (original_pts - av_rescale_q(original_pts, AVRational{multiplier.den, multiplier.num}, AVRational{1, 1})) : (av_rescale_q(original_pts, AVRational{multiplier.num, multiplier.den}, AVRational{1, 1}) - original_pts);

  return time_shift;
}

static const int64_t NEAR_ZERO_TIME_SHIFT_THRESHOLD = time_ms_to_av_time(0.5);

static bool compare_av_dictionaries(AVDictionary* dict1, AVDictionary* dict2) {
  if (av_dict_count(dict1) != av_dict_count(dict2)) {
    return false;
  }

  AVDictionaryEntry* entry1 = nullptr;
  AVDictionaryEntry* entry2 = nullptr;

  while ((entry1 = av_dict_get(dict1, "", entry1, AV_DICT_IGNORE_SUFFIX))) {
    entry2 = av_dict_get(dict2, entry1->key, nullptr, 0);
    if (!entry2 || std::string(entry1->value) != std::string(entry2->value)) {
      return false;
    }
  }

  return true;
}

static bool produces_same_decoded_video(const VideoCompareConfig& config) {
  return (config.left.file_name == config.right.file_name) && (config.left.demuxer == config.right.demuxer) && (config.left.decoder == config.right.decoder) && (config.left.hw_accel_spec == config.right.hw_accel_spec) &&
         compare_av_dictionaries(config.left.demuxer_options, config.right.demuxer_options) && compare_av_dictionaries(config.left.decoder_options, config.right.decoder_options) &&
         compare_av_dictionaries(config.left.hw_accel_options, config.right.hw_accel_options);
}

static inline AVPixelFormat determine_pixel_format(const VideoCompareConfig& config) {
  return config.use_10_bpc ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_RGB24;
}

static inline int determine_sws_flags(const bool fast) {
  return fast ? SWS_FAST_BILINEAR : (SWS_BICUBIC | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND);
}

static inline bool use_fast_input_alignment(const VideoCompareConfig& config) {
  return config.fast_input_alignment;
}

static void sleep_for_ms(const uint32_t ms) {
  std::chrono::milliseconds sleep(ms);
  std::this_thread::sleep_for(sleep);
}

VideoCompare::VideoCompare(const VideoCompareConfig& config)
    : same_decoded_video_both_sides_(produces_same_decoded_video(config)),
      auto_loop_mode_(config.auto_loop_mode),
      frame_buffer_size_(config.frame_buffer_size),
      time_shift_(config.time_shift),
      time_shift_offset_av_time_(time_ms_to_av_time(static_cast<double>(config.time_shift.offset_ms))),
      demuxers_{std::make_unique<Demuxer>(LEFT, config.left.demuxer, config.left.file_name, config.left.demuxer_options, config.left.decoder_options),
                std::make_unique<Demuxer>(RIGHT, config.right.demuxer, config.right.file_name, config.right.demuxer_options, config.right.decoder_options)},
      video_decoders_{
          std::make_unique<VideoDecoder>(LEFT, config.left.decoder, config.left.hw_accel_spec, demuxers_[LEFT]->video_codec_parameters(), config.left.peak_luminance_nits, config.left.hw_accel_options, config.left.decoder_options),
          std::make_unique<VideoDecoder>(RIGHT, config.right.decoder, config.right.hw_accel_spec, demuxers_[RIGHT]->video_codec_parameters(), config.right.peak_luminance_nits, config.right.hw_accel_options, config.right.decoder_options)},
      video_filterers_{std::make_unique<VideoFilterer>(LEFT,
                                                       demuxers_[LEFT].get(),
                                                       video_decoders_[LEFT].get(),
                                                       config.left.tone_mapping_mode,
                                                       config.left.boost_tone,
                                                       config.left.video_filters,
                                                       config.left.color_space,
                                                       config.left.color_range,
                                                       config.left.color_primaries,
                                                       config.left.color_trc,
                                                       demuxers_[RIGHT].get(),
                                                       video_decoders_[RIGHT].get(),
                                                       config.right.color_trc,
                                                       config.disable_auto_filters),
                       std::make_unique<VideoFilterer>(RIGHT,
                                                       demuxers_[RIGHT].get(),
                                                       video_decoders_[RIGHT].get(),
                                                       config.right.tone_mapping_mode,
                                                       config.right.boost_tone,
                                                       config.right.video_filters,
                                                       config.right.color_space,
                                                       config.right.color_range,
                                                       config.right.color_primaries,
                                                       config.right.color_trc,
                                                       demuxers_[LEFT].get(),
                                                       video_decoders_[LEFT].get(),
                                                       config.left.color_trc,
                                                       config.disable_auto_filters)},
      max_width_{std::max(video_filterers_[LEFT]->dest_width(), video_filterers_[RIGHT]->dest_width())},
      max_height_{std::max(video_filterers_[LEFT]->dest_height(), video_filterers_[RIGHT]->dest_height())},
      initial_fast_input_alignment_{use_fast_input_alignment(config)},
      shortest_duration_{std::min(demuxers_[LEFT]->duration(), demuxers_[RIGHT]->duration()) * AV_TIME_TO_SEC},
      format_converters_{std::make_unique<FormatConverter>(video_filterers_[LEFT]->dest_width(),
                                                           video_filterers_[LEFT]->dest_height(),
                                                           max_width_,
                                                           max_height_,
                                                           video_filterers_[LEFT]->dest_pixel_format(),
                                                           determine_pixel_format(config),
                                                           video_decoders_[LEFT]->color_space(),
                                                           video_decoders_[LEFT]->color_range(),
                                                           LEFT,
                                                           determine_sws_flags(initial_fast_input_alignment_)),
                         std::make_unique<FormatConverter>(video_filterers_[RIGHT]->dest_width(),
                                                           video_filterers_[RIGHT]->dest_height(),
                                                           max_width_,
                                                           max_height_,
                                                           video_filterers_[RIGHT]->dest_pixel_format(),
                                                           determine_pixel_format(config),
                                                           video_decoders_[RIGHT]->color_space(),
                                                           video_decoders_[RIGHT]->color_range(),
                                                           RIGHT,
                                                           determine_sws_flags(initial_fast_input_alignment_))},
      display_{std::make_unique<Display>(config.display_number,
                                         config.display_mode,
                                         config.verbose,
                                         config.fit_window_to_usable_bounds,
                                         config.high_dpi_allowed,
                                         config.use_10_bpc,
                                         initial_fast_input_alignment_,
                                         config.bilinear_texture_filtering,
                                         config.window_size,
                                         max_width_,
                                         max_height_,
                                         shortest_duration_,
                                         config.wheel_sensitivity,
                                         config.left.file_name,
                                         config.right.file_name)},
      timer_{std::make_unique<Timer>()},
      packet_queues_{std::make_unique<PacketQueue>(QUEUE_SIZE), std::make_unique<PacketQueue>(QUEUE_SIZE)},
      decoded_frame_queues_{std::make_unique<DecodedFrameQueue>(QUEUE_SIZE), std::make_unique<DecodedFrameQueue>(QUEUE_SIZE)},
      filtered_frame_queues_{std::make_unique<FrameQueue>(QUEUE_SIZE), std::make_unique<FrameQueue>(QUEUE_SIZE)},
      converted_frame_queues_{std::make_unique<FrameQueue>(QUEUE_SIZE), std::make_unique<FrameQueue>(QUEUE_SIZE)} {
  auto dump_video_info = [&](const Side side, const std::string& file_name) {
    const std::string dimensions = string_sprintf("%dx%d", video_decoders_[side]->width(), video_decoders_[side]->height());
    const std::string pixel_format_and_color_space =
        stringify_pixel_format(video_decoders_[side]->pixel_format(), video_decoders_[side]->color_range(), video_decoders_[side]->color_space(), video_decoders_[side]->color_primaries(), video_decoders_[side]->color_trc());

    std::string aspect_ratio;

    if (video_decoders_[side]->is_anamorphic()) {
      const AVRational display_aspect_ratio = video_decoders_[side]->display_aspect_ratio();

      aspect_ratio = string_sprintf(" [DAR %d:%d]", display_aspect_ratio.num, display_aspect_ratio.den);
    }

    // clang-format off
    auto info = string_sprintf(
      "Input: %9s%s, %s, %s, %s, %s, %s, %s, %s, %s, %s",
      dimensions.c_str(),
      aspect_ratio.c_str(),
      format_duration(demuxers_[side]->duration() * AV_TIME_TO_SEC).c_str(),
      stringify_frame_rate(demuxers_[side]->guess_frame_rate(), video_decoders_[side]->codec_context()->field_order).c_str(),
      stringify_decoder(video_decoders_[side].get()).c_str(),
      pixel_format_and_color_space.c_str(),
      demuxers_[side]->format_name().c_str(),
      file_name.c_str(),
      stringify_file_size(demuxers_[side]->file_size(), 2).c_str(),
      stringify_bit_rate(demuxers_[side]->bit_rate(), 1).c_str(),
      video_filterers_[side]->filter_description().c_str()
    );
    // clang-format on

    sa_log_info(side, info);
  };

  dump_video_info(LEFT, config.left.file_name.c_str());
  dump_video_info(RIGHT, config.right.file_name.c_str());

  // Initialize metadata overlay
  auto collect_metadata = [&](const Side side) -> VideoMetadata {
    VideoMetadata metadata;

    const std::string dimensions = string_sprintf("%dx%d", video_decoders_[side]->width(), video_decoders_[side]->height());
    metadata.set(MetadataProperties::RESOLUTION, dimensions);

    const AVRational sample_aspect_ratio = video_decoders_[side]->sample_aspect_ratio(true);
    const AVRational display_aspect_ratio = video_decoders_[side]->display_aspect_ratio();

    if (sample_aspect_ratio.num > 0) {
      metadata.set(MetadataProperties::SAMPLE_ASPECT_RATIO, string_sprintf("%d:%d", sample_aspect_ratio.num, sample_aspect_ratio.den));
      metadata.set(MetadataProperties::DISPLAY_ASPECT_RATIO, string_sprintf("%d:%d", display_aspect_ratio.num, display_aspect_ratio.den));
    } else {
      metadata.set(MetadataProperties::SAMPLE_ASPECT_RATIO, "unknown");
      metadata.set(MetadataProperties::DISPLAY_ASPECT_RATIO, "unknown");
    }

    metadata.set(MetadataProperties::CODEC, video_decoders_[side].get()->codec()->name);
    metadata.set(MetadataProperties::FRAME_RATE, stringify_frame_rate_only(demuxers_[side]->guess_frame_rate()));
    metadata.set(MetadataProperties::FIELD_ORDER, stringify_field_order(video_decoders_[side]->codec_context()->field_order, "unknown"));
    metadata.set(MetadataProperties::DURATION, format_duration(demuxers_[side]->duration() * AV_TIME_TO_SEC));
    metadata.set(MetadataProperties::BIT_RATE, stringify_bit_rate(demuxers_[side]->bit_rate(), 1));
    metadata.set(MetadataProperties::FILE_SIZE, stringify_file_size(demuxers_[side]->file_size(), 2));
    metadata.set(MetadataProperties::CONTAINER, demuxers_[side]->format_name());
    metadata.set(MetadataProperties::PIXEL_FORMAT, av_get_pix_fmt_name(video_decoders_[side]->pixel_format()));
    metadata.set(MetadataProperties::COLOR_SPACE, av_color_space_name(video_decoders_[side]->color_space()));
    metadata.set(MetadataProperties::COLOR_PRIMARIES, av_color_primaries_name(video_decoders_[side]->color_primaries()));
    metadata.set(MetadataProperties::TRANSFER_CURVE, av_color_transfer_name(video_decoders_[side]->color_trc()));
    metadata.set(MetadataProperties::COLOR_RANGE, av_color_range_name(video_decoders_[side]->color_range()));
    metadata.set(MetadataProperties::HARDWARE_ACCELERATION, video_decoders_[side]->is_hw_accelerated() ? video_decoders_[side]->hw_accel_name() : "None");
    metadata.set(MetadataProperties::FILTERS, video_filterers_[side]->filter_description());

    return metadata;
  };

  display_->update_metadata(collect_metadata(LEFT), collect_metadata(RIGHT));

  update_decoder_mode(time_shift_offset_av_time_);
}

void VideoCompare::operator()() {
  stages_.emplace_back(&VideoCompare::thread_demultiplex_left, this);
  stages_.emplace_back(&VideoCompare::thread_demultiplex_right, this);
  stages_.emplace_back(&VideoCompare::thread_decode_video_left, this);
  stages_.emplace_back(&VideoCompare::thread_decode_video_right, this);
  stages_.emplace_back(&VideoCompare::thread_filter_left, this);
  stages_.emplace_back(&VideoCompare::thread_filter_right, this);
  stages_.emplace_back(&VideoCompare::thread_format_converter_left, this);
  stages_.emplace_back(&VideoCompare::thread_format_converter_right, this);

  compare();

  for (auto& stage : stages_) {
    stage.join();
  }

  exception_holder_.rethrow_stored_exception();
}

void VideoCompare::thread_demultiplex_left() {
  demultiplex(LEFT);
}

void VideoCompare::thread_demultiplex_right() {
  demultiplex(RIGHT);
}

void VideoCompare::demultiplex(const Side side) {
  ScopedLogSide scoped_log_side(side);

  try {
    while (keep_running()) {
      // Wait for decoder to drain
      if (seeking_ && ready_to_seek_.get(ReadyToSeek::DECODER, side)) {
        ready_to_seek_.set(ReadyToSeek::DEMULTIPLEXER, side);

        sleep_for_ms(SLEEP_PERIOD_MS);
        continue;
      }
      // Sleep if we are finished for now
      if (packet_queues_[side]->is_stopped() || (side == RIGHT && single_decoder_mode_)) {
        sleep_for_ms(SLEEP_PERIOD_MS);
        continue;
      }

      // Create AVPacket
      AVPacketUniquePtr packet{new AVPacket, avpacket_deleter};
      av_init_packet(packet.get());
      packet->data = nullptr;

      // Read frame into AVPacket
      if (!(*demuxers_[side])(*packet)) {
        // Enter wait state if EOF
        packet_queues_[side]->stop();
        continue;
      }

      // Move into queue if first video stream
      if (packet->stream_index == demuxers_[side]->video_stream_index()) {
        packet_queues_[side]->push(std::move(packet));
      }
    }
  } catch (...) {
    exception_holder_.rethrow_stored_exception();
    quit_queues(side);
  }
}

void VideoCompare::thread_decode_video_left() {
  decode_video(LEFT);
}

void VideoCompare::thread_decode_video_right() {
  decode_video(RIGHT);
}

void VideoCompare::decode_video(const Side side) {
  ScopedLogSide scoped_log_side(side);

  try {
    while (keep_running()) {
      // Sleep if we are finished for now
      if (decoded_frame_queues_[side]->is_stopped() || (side == RIGHT && single_decoder_mode_)) {
        if (seeking_) {
          // Flush the decoder
          video_decoders_[side]->flush();

          // Seeks are now OK
          ready_to_seek_.set(ReadyToSeek::DECODER, side);
        }

        sleep_for_ms(SLEEP_PERIOD_MS);
        continue;
      }

      AVPacketUniquePtr packet{nullptr, avpacket_deleter};

      // Read packet from queue
      if (!packet_queues_[side]->pop(packet)) {
        // Flush remaining frames cached in the decoder
        while (process_packet(side, packet.get())) {
          ;
        }

        // Enter wait state
        decoded_frame_queues_[side]->stop();

        if (single_decoder_mode_) {
          decoded_frame_queues_[RIGHT]->stop();
        }
        continue;
      }

      // If the packet didn't send, receive more frames and try again
      while (!seeking_ && !process_packet(side, packet.get())) {
        ;
      }
    }
  } catch (...) {
    exception_holder_.rethrow_stored_exception();
    quit_queues(side);
  }
}

bool VideoCompare::process_packet(const Side side, AVPacket* packet) {
  bool sent = video_decoders_[side]->send(packet);

  while (true) {
    AVFrameSharedPtr frame_decoded{av_frame_alloc(), avframe_deleter};

    // If a whole frame has been decoded, adjust time stamps and add to queue
    if (!video_decoders_[side]->receive(frame_decoded.get(), demuxers_[side].get())) {
      break;
    }

    AVFrameSharedPtr frame_for_filtering;

    if (frame_decoded->format == video_decoders_[side]->hw_pixel_format()) {
      AVFrameSharedPtr sw_frame_decoded{av_frame_alloc(), avframe_deleter};

      // Transfer data from GPU to CPU
      if (av_hwframe_transfer_data(sw_frame_decoded.get(), frame_decoded.get(), 0) < 0) {
        throw std::runtime_error("Error transferring frame from GPU to CPU");
      }
      if (av_frame_copy_props(sw_frame_decoded.get(), frame_decoded.get()) < 0) {
        throw std::runtime_error("Copying SW frame properties");
      }

      frame_for_filtering = sw_frame_decoded;
    } else {
      frame_for_filtering = frame_decoded;
    }

    if (!decoded_frame_queues_[side]->push(frame_for_filtering)) {
      return sent;
    }

    // Send the decoded frame to the right filterer, as well, if in single decoder mode
    if (single_decoder_mode_) {
      decoded_frame_queues_[RIGHT]->push(frame_for_filtering);
    }
  }

  return sent;
}

void VideoCompare::thread_filter_left() {
  filter_video(LEFT);
}

void VideoCompare::thread_filter_right() {
  filter_video(RIGHT);
}

void VideoCompare::filter_video(const Side side) {
  ScopedLogSide scoped_log_side(side);

  try {
    while (keep_running()) {
      if (filtered_frame_queues_[side]->is_stopped()) {
        if (seeking_) {
          ready_to_seek_.set(ReadyToSeek::FILTERER, side);
        }

        sleep_for_ms(SLEEP_PERIOD_MS);
        continue;
      }

      AVFrameSharedPtr frame_to_filter;

      if (decoded_frame_queues_[side]->pop(frame_to_filter)) {
        filter_decoded_frame(side, frame_to_filter);
      } else if (decoded_frame_queues_[side]->is_stopped() || seeking_) {
        // Close the filter source
        video_filterers_[side]->close_src();

        // Flush the filter graph
        filter_decoded_frame(side, nullptr);

        // Stop filtering
        filtered_frame_queues_[side]->stop();
      }
    }
  } catch (...) {
    exception_holder_.rethrow_stored_exception();
    quit_queues(side);
  }
}

void VideoCompare::filter_decoded_frame(const Side side, AVFrameSharedPtr frame_decoded) {
  // send decoded frame to filterer
  if (!video_filterers_[side]->send(frame_decoded.get())) {
    throw std::runtime_error("Error while feeding the filter graph");
  }

  while (true) {
    AVFrameUniquePtr frame_filtered{av_frame_alloc(), avframe_deleter};

    // get next filtered frame
    if (!video_filterers_[side]->receive(frame_filtered.get())) {
      break;
    }

    if (!filtered_frame_queues_[side]->push(std::move(frame_filtered))) {
      return;
    }
  }

  return;
}

void VideoCompare::thread_format_converter_left() {
  format_convert_video(LEFT);
}

void VideoCompare::thread_format_converter_right() {
  format_convert_video(RIGHT);
}

void VideoCompare::format_convert_video(const Side side) {
  ScopedLogSide scoped_log_side(side);

  try {
    while (keep_running()) {
      if (converted_frame_queues_[side]->is_stopped()) {
        if (seeking_) {
          ready_to_seek_.set(ReadyToSeek::CONVERTER, side);
        }

        sleep_for_ms(SLEEP_PERIOD_MS);
        continue;
      }

      AVFrameUniquePtr frame_filtered{av_frame_alloc(), avframe_deleter};

      if (filtered_frame_queues_[side]->pop(frame_filtered)) {
        // scale and convert pixel format before pushing to frame queue for displaying
        AVFrameUniquePtr frame_converted{av_frame_alloc(), avframe_and_data_deleter};

        if (av_frame_copy_props(frame_converted.get(), frame_filtered.get()) < 0) {
          throw std::runtime_error("Copying filtered frame properties");
        }
        if (av_image_alloc(frame_converted->data, frame_converted->linesize, format_converters_[side]->dest_width(), format_converters_[side]->dest_height(), format_converters_[side]->dest_pixel_format(), 64) < 0) {
          throw std::runtime_error("Allocating converted picture");
        }
        (*format_converters_[side])(frame_filtered.get(), frame_converted.get());

        converted_frame_queues_[side]->push(std::move(frame_converted));
      } else if (filtered_frame_queues_[side]->is_stopped() || seeking_) {
        // Stop filtering
        converted_frame_queues_[side]->stop();
      }
    }
  } catch (...) {
    exception_holder_.rethrow_stored_exception();
    quit_queues(side);
  }
}

bool VideoCompare::keep_running() const {
  return !display_->get_quit() && !exception_holder_.has_exception();
}

void VideoCompare::quit_queues(const Side side) {
  converted_frame_queues_[side]->quit();
  filtered_frame_queues_[side]->quit();
  decoded_frame_queues_[side]->quit();
  packet_queues_[side]->quit();
}

void VideoCompare::update_decoder_mode(const int right_time_shift) {
  single_decoder_mode_ = same_decoded_video_both_sides_ && (av_q2d(time_shift_.multiplier) == 1.0) && (abs(right_time_shift) < NEAR_ZERO_TIME_SHIFT_THRESHOLD);
}

void VideoCompare::dump_debug_info(const int frame_number, const int effective_right_time_shift, const int average_refresh_time) {
  auto dump_queue_side = [&](const std::string& name, const std::string side_name, const Side side, const auto& queue) {
    std::cout << side_name << " " << name << ": size=" << queue[side]->size() << ", is_stopped=" << queue[side]->is_stopped() << ", quit=" << queue[side]->is_quit() << std::endl;
  };

  auto dump_queues = [&](const std::string& name, const auto& queue) {
    dump_queue_side(name, "Left", LEFT, queue);
    dump_queue_side(name, "Right", RIGHT, queue);
  };

  std::cout << "FRAME: " << frame_number << std::endl;
  std::cout << "keep_running()=" << keep_running() << std::endl;
  std::cout << "has_exception()=" << exception_holder_.has_exception() << std::endl;
  std::cout << "seeking=" << seeking_ << std::endl;
  std::cout << "effective_right_time_shift=" << effective_right_time_shift << std::endl;
  std::cout << "single_decoder_mode=" << single_decoder_mode_ << std::endl;
  std::cout << "average_refresh_time=" << average_refresh_time << std::endl;

  dump_queues("packet demuxer", packet_queues_);
  dump_queues("decoder", decoded_frame_queues_);
  dump_queues("filterer", filtered_frame_queues_);
  dump_queues("format converter", converted_frame_queues_);

  std::cout << "all_are_idle()=" << ready_to_seek_.all_are_idle() << std::endl;

  std::cout << "--------------------------------------------------" << std::endl;
}

struct SideState {
  SideState(const Side side, const std::string side_desc, const Demuxer* demuxer) : side_(side), side_desc_(std::move(side_desc)), start_time_(demuxer->start_time() * AV_TIME_TO_SEC), frame_duration_deque_(8) {
    if (start_time_ > 0) {
      sa_log_info(side, string_sprintf("Video has a start time of %s - timestamps will be shifted so they start at zero!", format_position(start_time_, true).c_str()));
    }
  }

  const Side side_;
  const std::string side_desc_;

  const float start_time_;

  std::deque<AVFrameUniquePtr> frames_;
  AVFrameUniquePtr frame_{nullptr, avframe_deleter};

  int64_t first_pts_ = 0;
  int64_t pts_ = 0;
  int64_t delta_pts_ = 0;
  int32_t previous_decoded_picture_number_ = -1;
  int32_t decoded_picture_number_ = 0;

  sorted_flat_deque<int64_t> frame_duration_deque_;
};

void VideoCompare::compare() {
  try {
#ifdef _DEBUG
    std::string previous_state;
#endif

    SideState left(LEFT, "left", demuxers_[LEFT].get());
    SideState right(RIGHT, "right", demuxers_[RIGHT].get());

    int frame_offset = 0;

    int64_t static_right_time_shift = time_shift_offset_av_time_;
    int64_t effective_right_time_shift = static_right_time_shift;
    int total_right_time_shifted = 0;

    int forward_navigate_frames = 0;

    bool auto_loop_triggered = false;

    const int max_digits = std::log10(frame_buffer_size_) + 1;
    const std::string frame_offset_format_str = string_sprintf("%%s%%0%dd/%%0%dd%%s", max_digits, max_digits);

    // for refreshing the display only
    Timer display_refresh_timer;
    sorted_flat_deque<uint32_t> refresh_time_deque(8);

    // for the full cycle
    Timer full_cycle_timer;
    sorted_flat_deque<uint32_t> full_cycle_time_deque(NOMINAL_FPS_UPDATE_RATE_US / 1000);

    int64_t previous_frame_combo_tag = -1;
    int32_t unique_frame_combo_tags_processed = 0;
    std::string fps_message = "Gathering stats... hold onto your pixels!";

    double next_refresh_at = 0;

    for (uint64_t frame_number = 0;; ++frame_number) {
      std::string message = display_->get_show_fps() ? fps_message : "";

      full_cycle_timer.update();

      // sample keyboard and mouse input events
      display_->input();

      if (!keep_running()) {
        break;
      }

#ifdef _DEBUG
      if ((frame_number % 100) == 0) {
        dump_debug_info(frame_number, effective_right_time_shift, refresh_time_deque.average());
      }
#endif

      const int format_conversion_sws_flags = determine_sws_flags(display_->get_fast_input_alignment());
      format_converters_[LEFT]->set_pending_flags(format_conversion_sws_flags);
      format_converters_[RIGHT]->set_pending_flags(format_conversion_sws_flags);

      // allow 50 ms of lag without resetting timer (and ticking playback)
      if (display_->get_tick_playback() || (display_->get_possibly_tick_playback() && (timer_->us_until_target() < -50000))) {
        timer_->reset();
      }

      forward_navigate_frames += display_->get_frame_navigation_delta();

      bool skip_update = false;

      if ((display_->get_seek_relative() != 0.0F) || (display_->get_shift_right_frames() != 0)) {
        total_right_time_shifted += display_->get_shift_right_frames();

        // compute effective time shift
        static_right_time_shift = time_shift_offset_av_time_ + total_right_time_shifted * (right.delta_pts_ > 0 ? right.delta_pts_ : 10000);

        ready_to_seek_.reset();
        seeking_ = true;

        // drain packet and frame queues
        auto stop_and_empty_packet_queue = [&](const Side side) {
          packet_queues_[side]->stop();
          packet_queues_[side]->empty();
        };

        stop_and_empty_packet_queue(LEFT);
        stop_and_empty_packet_queue(RIGHT);

        auto empty_frame_queues = [&]() {
          decoded_frame_queues_[LEFT]->empty();
          decoded_frame_queues_[RIGHT]->empty();
          filtered_frame_queues_[LEFT]->empty();
          filtered_frame_queues_[RIGHT]->empty();
          converted_frame_queues_[LEFT]->empty();
          converted_frame_queues_[RIGHT]->empty();
        };

        while (!ready_to_seek_.all_are_idle()) {
          empty_frame_queues();
          sleep_for_ms(SLEEP_PERIOD_MS);
#ifdef _DEBUG
          dump_debug_info(frame_number, effective_right_time_shift, refresh_time_deque.average());
#endif
        }

        // empty the frame queues one last time
        empty_frame_queues();

        // reinit filter graphs
        video_filterers_[LEFT]->reinit();
        video_filterers_[RIGHT]->reinit();

        update_decoder_mode(static_right_time_shift);

        float next_left_position, next_right_position;

        // the left video is the "master"
        const float left_position = left.pts_ * AV_TIME_TO_SEC + left.start_time_;
        const float right_position = left.pts_ * AV_TIME_TO_SEC + right.start_time_;

        if (display_->get_seek_from_start()) {
          // seek from start based on the shortest stream duration in seconds
          next_left_position = shortest_duration_ * display_->get_seek_relative() + left.start_time_;
          next_right_position = shortest_duration_ * display_->get_seek_relative() + right.start_time_;
        } else {
          next_left_position = left_position + display_->get_seek_relative();
          next_right_position = right_position + display_->get_seek_relative();
        }

        next_right_position += (static_right_time_shift + right.delta_pts_) * AV_TIME_TO_SEC;
        next_right_position += static_cast<float>(calculate_dynamic_time_shift(time_shift_.multiplier, (next_right_position - right.start_time_) / AV_TIME_TO_SEC, false)) * AV_TIME_TO_SEC;

        const bool backward = (display_->get_seek_relative() < 0.0F) || (display_->get_shift_right_frames() != 0);

#ifdef _DEBUG
        std::cout << "SEEK: next_left_position=" << (int)(next_left_position * 1000) << ", next_right_position=" << (int)(next_right_position * 1000) << ", backward=" << backward << std::endl;
#endif
        const bool left_seek_result = demuxers_[LEFT]->seek(next_left_position, backward);
        const bool right_seek_result = demuxers_[RIGHT]->seek(next_right_position, backward);

        if ((!left_seek_result && !backward) || (!right_seek_result && !backward)) {
          // restore position if unable to perform forward seek
          message = "Unable to seek past end of file";

          demuxers_[LEFT]->seek(left_position, true);
          demuxers_[RIGHT]->seek(right_position, true);
        };

        seeking_ = false;

        // allow packet and frame queues to receive data again
        auto reset_queues = [&](const Side side) {
          packet_queues_[side]->restart();
          decoded_frame_queues_[side]->restart();
          filtered_frame_queues_[side]->restart();
          converted_frame_queues_[side]->restart();
        };

        reset_queues(LEFT);
        reset_queues(RIGHT);

        auto pop_and_reset = [&](SideState& side_state, int64_t* effective_time_shift = nullptr) {
          converted_frame_queues_[side_state.side_]->pop(side_state.frame_);

          if (side_state.frame_ != nullptr) {
            side_state.pts_ = side_state.frame_->pts;

            // if the effective time shift is provided, update it and subtract it from the PTS
            if (effective_time_shift != nullptr) {
              *effective_time_shift += calculate_dynamic_time_shift(time_shift_.multiplier, side_state.frame_->pts, true);
              side_state.pts_ -= *effective_time_shift;
            }

            side_state.previous_decoded_picture_number_ = -1;
            side_state.decoded_picture_number_ = 1;

            side_state.frames_.clear();
          }
        };

        pop_and_reset(left);

        // round away from zero to nearest 2 ms
        if (static_right_time_shift > 0) {
          static_right_time_shift = ((static_right_time_shift / 1000) + 2) * 1000;
        } else if (static_right_time_shift < 0) {
          static_right_time_shift = ((static_right_time_shift / 1000) - 2) * 1000;
        }

        effective_right_time_shift = static_right_time_shift;
        pop_and_reset(right, &effective_right_time_shift);

        // don't sync until the next iteration to prevent freezing when comparing a single image
        skip_update = true;
      }

      bool store_frames = false;
      bool adjusting = false;

      // keep showing currently displayed frame for another iteration?
      skip_update = skip_update || (timer_->us_until_target() - refresh_time_deque.average()) > 0;
      const bool fetch_next_frame = display_->get_play() || (forward_navigate_frames > 0);

      // use the delta between current and previous PTS as the tolerance which determines whether we have to adjust
      const int64_t min_delta = compute_min_delta(left.delta_pts_, right.delta_pts_);

#ifdef _DEBUG
      const std::string current_state = string_sprintf("left_pts=%5d, left_is_behind=%d, right_pts=%5d, right_is_behind=%d, min_delta=%5d, effective_right_time_shift=%5d", left.pts_ / 1000, is_behind(left.pts_, right.pts_, min_delta),
                                                       (right.pts_ + static_right_time_shift) / 1000, is_behind(right.pts_, left.pts_, min_delta), min_delta / 1000, effective_right_time_shift / 1000);

      if (current_state != previous_state) {
        std::cout << current_state << std::endl;
      }

      previous_state = current_state;
#endif
      auto pop_frame = [&](SideState& side_state) {
        const bool result = converted_frame_queues_[side_state.side_]->pop(side_state.frame_);

        if (result) {
          side_state.decoded_picture_number_++;
        }

        return result;
      };
      auto sync_frame_queue = [&](SideState& side_state, const SideState& other_side) {
        if (is_behind(side_state.pts_, other_side.pts_, min_delta)) {
          adjusting = true;

          pop_frame(side_state);
        }
      };

      sync_frame_queue(left, right);
      sync_frame_queue(right, left);

      // handle regular playback only
      if (!skip_update && display_->get_buffer_play_loop_mode() == Display::Loop::OFF) {
        if (!adjusting && fetch_next_frame) {
          if (!pop_frame(left) || !pop_frame(right)) {
            left.frame_ = nullptr;
            right.frame_ = nullptr;

            timer_->update();
          } else {
            store_frames = true;

            effective_right_time_shift = static_right_time_shift + calculate_dynamic_time_shift(time_shift_.multiplier, right.frame_->pts, true);

            // update timer for regular playback
            if (frame_number > 0) {
              const int64_t play_frame_delay = compute_frame_delay(left.frame_->pts - left.pts_, right.frame_->pts - right.pts_ - effective_right_time_shift);

              timer_->shift_target(play_frame_delay / display_->get_playback_speed_factor());
            } else {
              left.first_pts_ = left.frame_->pts;
              right.first_pts_ = right.frame_->pts;

              timer_->update();
            }
          }
        } else {
          timer_->reset();
        }
      }

      // for frame-accurate forward navigation, decrement counter when frame is stored in buffer
      if (store_frames && (forward_navigate_frames > 0)) {
        forward_navigate_frames--;
      }

      auto update_frame_timing = [](SideState& side_state, const int64_t& time_shift) {
        if (side_state.frame_ != nullptr) {
          // determine time-shifted PTS (note: new_pts only differs from frame->pts on the right side)
          const int64_t new_pts = side_state.frame_->pts - time_shift;

          if ((side_state.decoded_picture_number_ - side_state.previous_decoded_picture_number_) == 1) {
            // compute the average PTS delta in a rolling-window fashion
            const int64_t last_duration = new_pts - side_state.pts_;
            side_state.frame_duration_deque_.push_back(last_duration);
            side_state.delta_pts_ = side_state.frame_duration_deque_.average();
          }

          if (side_state.delta_pts_ > 0) {
            // use the average PTS delta for frame duration
            ffmpeg::frame_duration(side_state.frame_.get()) = side_state.delta_pts_;

            if (!side_state.frames_.empty() && side_state.frames_.back()->pts == side_state.first_pts_) {
              // update the duration of the first stored frame once the second frame has been decoded
              ffmpeg::frame_duration(side_state.frames_.back().get()) = side_state.delta_pts_;
            }
          } else {
            side_state.delta_pts_ = ffmpeg::frame_duration(side_state.frame_.get());
          }

          side_state.pts_ = new_pts;
          side_state.previous_decoded_picture_number_ = side_state.decoded_picture_number_;
        }
      };

      update_frame_timing(left, 0);
      update_frame_timing(right, effective_right_time_shift);

      auto manage_frame_buffer = [&](SideState& side_state) {
        auto& frame = side_state.frame_;
        auto& frames = side_state.frames_;

        if (store_frames) {
          if (frames.size() >= frame_buffer_size_) {
            frames.pop_back();
          }
          frames.push_front(std::move(frame));
        } else if (frame != nullptr) {
          if (!frames.empty()) {
            frames.front() = std::move(frame);
          } else {
            frames.push_front(std::move(frame));
          }
        }
      };

      manage_frame_buffer(left);
      manage_frame_buffer(right);

      const bool no_activity = !skip_update && !adjusting && !store_frames;
      const bool end_of_file = no_activity && (converted_frame_queues_[LEFT]->is_stopped() || converted_frame_queues_[RIGHT]->is_stopped());
      const bool buffer_is_full = left.frames_.size() == frame_buffer_size_ && right.frames_.size() == frame_buffer_size_;

      const int last_common_frame_index = static_cast<int>(std::min(left.frames_.size(), right.frames_.size()) - 1);

      auto adjust_frame_offset = [last_common_frame_index](const int frame_offset, const int adjustment) { return std::min(std::max(0, frame_offset + adjustment), last_common_frame_index); };

      frame_offset = adjust_frame_offset(frame_offset, display_->get_frame_buffer_offset_delta());

      bool ui_refresh_performed = false;

      if (frame_offset >= 0 && !left.frames_.empty() && !right.frames_.empty()) {
        const bool is_playback_in_sync = is_in_sync(left.pts_, right.pts_, left.delta_pts_, right.delta_pts_);

        // reduce refresh rate to 10 Hz for faster re-syncing
        const bool skip_refresh = !is_playback_in_sync && display_refresh_timer.us_until_target() > -RESYNC_UPDATE_RATE_US;

        if (!skip_refresh) {
          const auto& left_frames_ref = !display_->get_swap_left_right() ? left.frames_ : right.frames_;
          const auto& right_frames_ref = !display_->get_swap_left_right() ? right.frames_ : left.frames_;

          const auto left_display_frame = left_frames_ref[frame_offset].get();
          const auto right_display_frame = right_frames_ref[frame_offset].get();

          // count the number of unique in-sync video frame combinations processed
          if (is_playback_in_sync) {
            const int64_t frame_combo_tag = (left_display_frame->pts << 20) | right_display_frame->pts;

            if (frame_combo_tag != previous_frame_combo_tag) {
              unique_frame_combo_tags_processed++;
              previous_frame_combo_tag = frame_combo_tag;
            }
          }

          // conditionally refresh display in an attempt to keep up with the target playback speed
          const uint64_t next_refresh_frame_number = lrintf(next_refresh_at);

          if (frame_number >= next_refresh_frame_number) {
            std::string prefix_str, suffix_str;

            // add [] to the current / total browsable string when in sync
            if (fetch_next_frame && is_playback_in_sync) {
              prefix_str = "[";
              suffix_str = "]";
            }

            const std::string current_total_browsable = string_sprintf(frame_offset_format_str.c_str(), prefix_str.c_str(), frame_offset + 1, last_common_frame_index + 1, suffix_str.c_str());

            // conditionally update the display; otherwise, sleep to conserve resources
            display_refresh_timer.update();

            if (display_->possibly_refresh(left_display_frame, right_display_frame, current_total_browsable, message)) {
              refresh_time_deque.push_back(-display_refresh_timer.us_until_target());
            } else {
              sleep_for_ms(refresh_time_deque.average() / 1000);
            }

            ui_refresh_performed = true;

            // calculate next refresh time dynamically based on target playback speed and current refresh timing
            const double target_time_us = std::max(1000.0, static_cast<double>(std::max(ffmpeg::frame_duration(left_display_frame), ffmpeg::frame_duration(right_display_frame))) / display_->get_playback_speed_factor());
            const double refresh_time_us = static_cast<double>(refresh_time_deque.average());

            next_refresh_at += std::max(1.0 + (frame_number - next_refresh_frame_number), refresh_time_us / target_time_us);
          }

          // check if sleeping is the best option for accurate playback by taking the average refresh time into account
          const int64_t time_until_final_refresh = timer_->us_until_target();

          if (!adjusting && time_until_final_refresh > 0 && time_until_final_refresh < refresh_time_deque.average()) {
            timer_->wait(time_until_final_refresh);
          } else if (time_until_final_refresh <= 0 && display_->get_buffer_play_loop_mode() != Display::Loop::OFF) {
            // auto-adjust current frame during in-buffer playback
            switch (display_->get_buffer_play_loop_mode()) {
              case Display::Loop::FORWARDONLY:
                if (frame_offset == 0) {
                  frame_offset = last_common_frame_index;
                } else {
                  frame_offset = adjust_frame_offset(frame_offset, -1);
                }
                break;
              case Display::Loop::PINGPONG:
                if (last_common_frame_index >= 1 && (frame_offset == 0 || frame_offset == last_common_frame_index)) {
                  display_->toggle_buffer_play_direction();
                }
                frame_offset = adjust_frame_offset(frame_offset, display_->get_buffer_play_forward() ? -1 : 1);
                break;
              default:
                break;
            }

            // update timer for accurate in-buffer playback
            const int64_t in_buffer_frame_delay = compute_frame_delay(ffmpeg::frame_duration(left.frames_[frame_offset].get()), ffmpeg::frame_duration(right.frames_[frame_offset].get()));

            timer_->shift_target(in_buffer_frame_delay / display_->get_playback_speed_factor());
          }

          // enter in-buffer playback once if buffer is full or EOF reached
          if (auto_loop_mode_ != Display::Loop::OFF && !auto_loop_triggered && (buffer_is_full || end_of_file)) {
            display_->set_buffer_play_loop_mode(auto_loop_mode_);

            auto_loop_triggered = true;
          }
        }
      }

      if (ui_refresh_performed) {
        full_cycle_time_deque.push_back(-full_cycle_timer.us_until_target());

        // update video/UI frame rate string every second (or if deque gets full)
        if ((full_cycle_time_deque.sum() > NOMINAL_FPS_UPDATE_RATE_US) || full_cycle_time_deque.full()) {
          auto calculate_fps = [](const uint32_t num, const uint32_t denom) { return static_cast<float>(num) / static_cast<float>(denom); };

          const float video_fps = calculate_fps(ONE_SECOND_US * unique_frame_combo_tags_processed, full_cycle_time_deque.sum());
          const float ui_fps = calculate_fps(ONE_SECOND_US, full_cycle_time_deque.average());

          fps_message = string_sprintf("Video/UI FPS: %.1f/%.1f", video_fps, ui_fps);

          full_cycle_time_deque.clear();
          unique_frame_combo_tags_processed = 0;
        }
      }
    }
  } catch (...) {
    exception_holder_.store_current_exception();
  }

  quit_queues(LEFT);
  quit_queues(RIGHT);
}
