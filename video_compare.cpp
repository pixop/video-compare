#include "video_compare.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <thread>
#include "ffmpeg.h"
#include "sorted_flat_deque.h"
#include "string_utils.h"
extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
}

const size_t VideoCompare::QUEUE_SIZE{5};

static inline bool is_behind(int64_t frame1_pts, int64_t frame2_pts, int64_t delta_pts) {
  float t1 = static_cast<float>(frame1_pts) * AV_TIME_TO_SEC;
  float t2 = static_cast<float>(frame2_pts) * AV_TIME_TO_SEC;
  float delta_s = static_cast<float>(delta_pts) * AV_TIME_TO_SEC;

  float diff = t1 - t2;
  float tolerance = std::max(delta_s, 1.0F / 120.0F);

  return diff < -tolerance;
}

VideoCompare::VideoCompare(const int display_number,
                           const Display::Mode display_mode,
                           const bool high_dpi_allowed,
                           const bool use_10_bpc,
                           const std::tuple<int, int> window_size,
                           const double time_shift_ms,
                           const std::string& left_file_name,
                           const std::string& left_video_filters,
                           const std::string& left_demuxer,
                           const std::string& left_decoder,
                           const std::string& right_file_name,
                           const std::string& right_video_filters,
                           const std::string& right_demuxer,
                           const std::string& right_decoder)
    : time_shift_ms_(time_shift_ms),
      demuxer_{std::make_unique<Demuxer>(left_demuxer, left_file_name), std::make_unique<Demuxer>(right_demuxer, right_file_name)},
      video_decoder_{std::make_unique<VideoDecoder>(left_decoder, demuxer_[0]->video_codec_parameters()), std::make_unique<VideoDecoder>(right_decoder, demuxer_[1]->video_codec_parameters())},
      video_filterer_{std::make_unique<VideoFilterer>(demuxer_[0].get(), video_decoder_[0].get(), left_video_filters), std::make_unique<VideoFilterer>(demuxer_[1].get(), video_decoder_[1].get(), right_video_filters)},
      max_width_{std::max(video_filterer_[0]->dest_width(), video_filterer_[1]->dest_width())},
      max_height_{std::max(video_filterer_[0]->dest_height(), video_filterer_[1]->dest_height())},
      shortest_duration_{std::min(demuxer_[0]->duration(), demuxer_[1]->duration()) * AV_TIME_TO_SEC},
      format_converter_{
          std::make_unique<FormatConverter>(video_filterer_[0]->dest_width(), video_filterer_[0]->dest_height(), max_width_, max_height_, video_filterer_[0]->dest_pixel_format(), use_10_bpc ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_RGB24),
          std::make_unique<FormatConverter>(video_filterer_[1]->dest_width(), video_filterer_[1]->dest_height(), max_width_, max_height_, video_filterer_[1]->dest_pixel_format(), use_10_bpc ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_RGB24)},
      display_{std::make_unique<Display>(display_number, display_mode, high_dpi_allowed, use_10_bpc, window_size, max_width_, max_height_, shortest_duration_, left_file_name, right_file_name)},
      timer_{std::make_unique<Timer>()},
      packet_queue_{std::make_unique<PacketQueue>(QUEUE_SIZE), std::make_unique<PacketQueue>(QUEUE_SIZE)},
      frame_queue_{std::make_unique<FrameQueue>(QUEUE_SIZE), std::make_unique<FrameQueue>(QUEUE_SIZE)} {
  std::cout << string_sprintf("Left video:  %dx%d, %s, %s, %s, %s, %s", video_decoder_[0]->width(), video_decoder_[0]->height(), format_position(demuxer_[0]->duration() * AV_TIME_TO_SEC, false).c_str(), video_decoder_[0]->codec()->name,
                              av_get_pix_fmt_name(video_decoder_[0]->pixel_format()), demuxer_[0]->format_name().c_str(), left_file_name.c_str())
            << std::endl;
  std::cout << string_sprintf("Right video: %dx%d, %s, %s, %s, %s, %s", video_decoder_[1]->width(), video_decoder_[1]->height(), format_position(demuxer_[1]->duration() * AV_TIME_TO_SEC, false).c_str(), video_decoder_[1]->codec()->name,
                              av_get_pix_fmt_name(video_decoder_[1]->pixel_format()), demuxer_[1]->format_name().c_str(), right_file_name.c_str())
            << std::endl;
}

void VideoCompare::operator()() {
  stages_.emplace_back(&VideoCompare::thread_demultiplex_left, this);
  stages_.emplace_back(&VideoCompare::thread_demultiplex_right, this);
  stages_.emplace_back(&VideoCompare::thread_decode_video_left, this);
  stages_.emplace_back(&VideoCompare::thread_decode_video_right, this);
  video();

  for (auto& stage : stages_) {
    stage.join();
  }

  if (exception_) {
    std::rethrow_exception(exception_);
  }
}

void VideoCompare::thread_demultiplex_left() {
  demultiplex(0);
}

void VideoCompare::thread_demultiplex_right() {
  demultiplex(1);
}

void VideoCompare::demultiplex(const int video_idx) {
  try {
    for (;;) {
      if (seeking_ && readyToSeek_[1][video_idx]) {
        readyToSeek_[0][video_idx] = true;

        std::chrono::milliseconds sleep(10);
        std::this_thread::sleep_for(sleep);
        continue;
      }

      // Create AVPacket
      std::unique_ptr<AVPacket, std::function<void(AVPacket*)>> packet{new AVPacket, [](AVPacket* p) {
                                                                         av_packet_unref(p);
                                                                         delete p;
                                                                       }};
      av_init_packet(packet.get());
      packet->data = nullptr;

      // Read frame into AVPacket
      if (!(*demuxer_[video_idx])(*packet)) {
        packet_queue_[video_idx]->finished();
        break;
      }

      // Move into queue if first video stream
      if (packet->stream_index == demuxer_[video_idx]->video_stream_index()) {
        if (!packet_queue_[video_idx]->push(std::move(packet))) {
          break;
        }
      }
    }
  } catch (...) {
    exception_ = std::current_exception();
    frame_queue_[video_idx]->quit();
    packet_queue_[video_idx]->quit();
  }
}

void VideoCompare::thread_decode_video_left() {
  decode_video(0);
}

void VideoCompare::thread_decode_video_right() {
  decode_video(1);
}

bool VideoCompare::process_packet(const int video_idx, AVPacket* packet, AVFrame* frame_decoded) {
  bool sent = video_decoder_[video_idx]->send(packet);

  // If a whole frame has been decoded, adjust time stamps and add to queue
  while (video_decoder_[video_idx]->receive(frame_decoded)) {
    // send decoded frame to filterer
    if (!video_filterer_[video_idx]->send(frame_decoded)) {
      throw std::runtime_error("Error while feeding the filter graph");
    }

    std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame_filtered{av_frame_alloc(), [](AVFrame* f) { av_free(f->data[0]); }};

    while (true) {
      // get next filtered frame
      if (!video_filterer_[video_idx]->receive(frame_filtered.get())) {
        break;
      }

      // scale and convert pixel format before pushing to frame queue for displaying
      std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame_converted{av_frame_alloc(), [](AVFrame* f) { av_free(f->data[0]); }};

      if (av_frame_copy_props(frame_converted.get(), frame_filtered.get()) < 0) {
        throw std::runtime_error("Copying filtered frame properties");
      }
      if (av_image_alloc(frame_converted->data, frame_converted->linesize, format_converter_[video_idx]->dest_width(), format_converter_[video_idx]->dest_height(), format_converter_[video_idx]->output_pixel_format(), 1) < 0) {
        throw std::runtime_error("Allocating converted picture");
      }
      (*format_converter_[video_idx])(frame_filtered.get(), frame_converted.get());

      av_frame_unref(frame_filtered.get());

      if (!frame_queue_[video_idx]->push(std::move(frame_converted))) {
        return sent;
      }
    }
  }

  return sent;
}

void VideoCompare::decode_video(const int video_idx) {
  try {
    for (;;) {
      // Create AVFrame and AVPacket
      std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame_decoded{av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); }};
      std::unique_ptr<AVPacket, std::function<void(AVPacket*)>> packet{nullptr, [](AVPacket* p) {
                                                                         av_packet_unref(p);
                                                                         delete p;
                                                                       }};

      // Read packet from queue
      if (!packet_queue_[video_idx]->pop(packet)) {
        // Flush remaining frames cached in the decoder
        while (process_packet(video_idx, packet.get(), frame_decoded.get())) {
          ;
        }

        frame_queue_[video_idx]->finished();
        break;
      }

      if (seeking_) {
        video_decoder_[video_idx]->flush();

        readyToSeek_[1][video_idx] = true;

        std::chrono::milliseconds sleep(10);
        std::this_thread::sleep_for(sleep);
        continue;
      }

      // If the packet didn't send, receive more frames and try again
      while (!process_packet(video_idx, packet.get(), frame_decoded.get()) && !seeking_) {
        ;
      }
    }
  } catch (...) {
    exception_ = std::current_exception();
    frame_queue_[video_idx]->quit();
    packet_queue_[video_idx]->quit();
  }
}

void VideoCompare::video() {
  try {
    std::deque<std::unique_ptr<AVFrame, std::function<void(AVFrame*)>>> left_frames;
    std::deque<std::unique_ptr<AVFrame, std::function<void(AVFrame*)>>> right_frames;
    int frame_offset = 0;

    std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame_left{nullptr, [](AVFrame* f) { av_frame_free(&f); }};
    std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame_right{nullptr, [](AVFrame* f) { av_frame_free(&f); }};

    int64_t left_pts = 0;
    int64_t left_decoded_picture_number = 0;
    int64_t left_previous_decoded_picture_number = -1;
    int64_t delta_left_pts = 0;
    float left_start_time = demuxer_[0]->start_time() * AV_TIME_TO_SEC;

    if (left_start_time > 0) {
      std::cout << "Note: The left video has a start time of " << format_position(left_start_time, true) << " - timestamps will be shifted so they start at zero!" << std::endl;
    }

    int64_t right_pts = 0;
    int64_t right_decoded_picture_number = 0;
    int64_t right_previous_decoded_picture_number = -1;
    int64_t delta_right_pts = 0;
    float right_start_time = demuxer_[1]->start_time() * AV_TIME_TO_SEC;

    if (right_start_time > 0) {
      std::cout << "Note: The right video has a start time of " << format_position(right_start_time, true) << " - timestamps will be shifted so they start at zero!" << std::endl;
    }

    sorted_flat_deque<int32_t> left_deque(8);
    sorted_flat_deque<int32_t> right_deque(8);

    int64_t right_time_shift = time_shift_ms_ * MILLISEC_TO_AV_TIME;
    int total_right_time_shifted = 0;

    for (uint64_t frame_number = 0;; ++frame_number) {
      std::string error_message;

      display_->input();

      float current_position = left_pts * AV_TIME_TO_SEC;

      if ((display_->get_seek_relative() != 0.0F) || (display_->get_shift_right_frames() != 0)) {
        total_right_time_shifted += display_->get_shift_right_frames();

        if (packet_queue_[0]->is_finished() || packet_queue_[1]->is_finished()) {
          error_message = "Unable to perform seek (end of file reached)";
        } else {
          // compute effective time shift
          right_time_shift = time_shift_ms_ * MILLISEC_TO_AV_TIME + total_right_time_shifted * (delta_right_pts > 0 ? delta_right_pts : 10000);

          seeking_ = true;
          readyToSeek_[0][0] = false;
          readyToSeek_[0][1] = false;
          readyToSeek_[1][0] = false;
          readyToSeek_[1][1] = false;

          while (true) {
            bool all_empty = true;

            all_empty = all_empty && readyToSeek_[0][0];
            all_empty = all_empty && readyToSeek_[0][1];
            all_empty = all_empty && readyToSeek_[1][0];
            all_empty = all_empty && readyToSeek_[1][1];

            if (all_empty) {
              break;
            }
            frame_queue_[0]->empty();
            frame_queue_[1]->empty();
          }

          packet_queue_[0]->empty();
          packet_queue_[1]->empty();
          frame_queue_[0]->empty();
          frame_queue_[1]->empty();
          video_filterer_[0]->reinit();
          video_filterer_[1]->reinit();

          float next_position;

          if (display_->get_seek_from_start()) {
            // seek from start based on the shortest stream duration in seconds
            next_position = shortest_duration_ * display_->get_seek_relative();
          } else {
            next_position = current_position + display_->get_seek_relative();
          }

          bool backward = (display_->get_seek_relative() < 0.0F) || (display_->get_shift_right_frames() != 0);

          if ((!demuxer_[0]->seek(std::max(0.0F, next_position + left_start_time), backward) && !backward) || (!demuxer_[1]->seek(std::max(0.0F, next_position + right_start_time), backward) && !backward)) {
            // restore position if unable to perform forward seek
            error_message = "Unable to seek past end of file";
            demuxer_[0]->seek(std::max(0.0F, current_position + left_start_time), true);
            demuxer_[1]->seek(std::max(0.0F, current_position + right_start_time), true);
          };

          seeking_ = false;

          frame_queue_[0]->pop(frame_left);
          left_pts = frame_left->pts;
          left_previous_decoded_picture_number = -1;
          left_decoded_picture_number = 1;

          // round away from zero to nearest 2 ms (except when the left PTS is 0 then round towards zero)
          if (right_time_shift > 0) {
            right_time_shift = ((right_time_shift / 1000) + (left_pts > 0 ? 2 : -2)) * 1000;
          } else if (right_time_shift < 0) {
            right_time_shift = ((right_time_shift / 1000) - 2) * 1000;
          }

          frame_queue_[1]->pop(frame_right);
          right_pts = frame_right->pts - right_time_shift;
          right_previous_decoded_picture_number = -1;
          right_decoded_picture_number = 1;

          left_frames.clear();
          right_frames.clear();

          current_position = frame_left->pts * AV_TIME_TO_SEC;
        }
      }

      bool store_frames = false;

      if (display_->get_quit() || (exception_ != nullptr)) {
        break;
      } else {
        bool adjusting = false;

        // use the delta between current and previous PTS as the tolerance which determines whether we have to adjust
        if ((left_pts < 0) || is_behind(left_pts, right_pts, delta_left_pts)) {
          adjusting = true;

          if (frame_queue_[0]->pop(frame_left)) {
            left_decoded_picture_number++;
          }
        }
        if ((right_pts < 0) || is_behind(right_pts, left_pts, delta_right_pts)) {
          adjusting = true;

          if (frame_queue_[1]->pop(frame_right)) {
            right_decoded_picture_number++;
          }
        }

        if (!adjusting && display_->get_play()) {
          if (!frame_queue_[0]->pop(frame_left) || !frame_queue_[1]->pop(frame_right)) {
            if (frame_left != nullptr) {
              left_decoded_picture_number++;
            }
            if (frame_right != nullptr) {
              right_decoded_picture_number++;
            }

            timer_->update();
          } else {
            left_decoded_picture_number++;
            right_decoded_picture_number++;

            store_frames = true;

            if (frame_number > 0) {
              const int64_t frame_delay = frame_left->pts - left_pts;
              timer_->wait(frame_delay);
            } else {
              timer_->update();
            }
          }
        } else {
          timer_->update();
        }
      }

      if (frame_left != nullptr) {
        if ((left_decoded_picture_number - left_previous_decoded_picture_number) == 1) {
          left_deque.push_back(frame_left->pts - left_pts);
          delta_left_pts = left_deque.average();
        }

        left_pts = frame_left->pts;
        left_previous_decoded_picture_number = left_decoded_picture_number;
      }
      if (frame_right != nullptr) {
        float new_right_pts = frame_right->pts - right_time_shift;

        if ((right_decoded_picture_number - right_previous_decoded_picture_number) == 1) {
          right_deque.push_back(new_right_pts - right_pts);
          delta_right_pts = right_deque.average();
        }

        right_pts = new_right_pts;
        right_previous_decoded_picture_number = right_decoded_picture_number;
      }

      if (store_frames) {
        if (left_frames.size() >= 50) {
          left_frames.pop_back();
        }
        if (right_frames.size() >= 50) {
          right_frames.pop_back();
        }

        left_frames.push_front(std::move(frame_left));
        right_frames.push_front(std::move(frame_right));
      } else {
        if (frame_left != nullptr) {
          if (!left_frames.empty()) {
            left_frames[0] = std::move(frame_left);
          } else {
            left_frames.push_front(std::move(frame_left));
          }
        }
        if (frame_right != nullptr) {
          if (!right_frames.empty()) {
            right_frames[0] = std::move(frame_right);
          } else {
            right_frames.push_front(std::move(frame_right));
          }
        }
      }

      frame_offset = std::min(std::max(0, frame_offset + display_->get_frame_offset_delta()), static_cast<int>(left_frames.size()) - 1);

      const std::string current_total_browsable = string_sprintf("%d/%d", frame_offset + 1, static_cast<int>(left_frames.size()));

      if (frame_offset >= 0 && !left_frames.empty() && !right_frames.empty()) {
        if (!display_->get_swap_left_right()) {
          display_->refresh({left_frames[frame_offset]->data[0], left_frames[frame_offset]->data[1], left_frames[frame_offset]->data[2]},
                            {static_cast<size_t>(left_frames[frame_offset]->linesize[0]), static_cast<size_t>(left_frames[frame_offset]->linesize[1]), static_cast<size_t>(left_frames[frame_offset]->linesize[2])},
                            {video_decoder_[0]->width(), video_decoder_[0]->height()}, {right_frames[frame_offset]->data[0], right_frames[frame_offset]->data[1], right_frames[frame_offset]->data[2]},
                            {static_cast<size_t>(right_frames[frame_offset]->linesize[0]), static_cast<size_t>(right_frames[frame_offset]->linesize[1]), static_cast<size_t>(right_frames[frame_offset]->linesize[2])},
                            {video_decoder_[1]->width(), video_decoder_[1]->height()}, left_frames[frame_offset].get(), right_frames[frame_offset].get(), current_total_browsable, error_message);
        } else {
          display_->refresh({right_frames[frame_offset]->data[0], right_frames[frame_offset]->data[1], right_frames[frame_offset]->data[2]},
                            {static_cast<size_t>(right_frames[frame_offset]->linesize[0]), static_cast<size_t>(right_frames[frame_offset]->linesize[1]), static_cast<size_t>(right_frames[frame_offset]->linesize[2])},
                            {video_decoder_[1]->width(), video_decoder_[1]->height()}, {left_frames[frame_offset]->data[0], left_frames[frame_offset]->data[1], left_frames[frame_offset]->data[2]},
                            {static_cast<size_t>(left_frames[frame_offset]->linesize[0]), static_cast<size_t>(left_frames[frame_offset]->linesize[1]), static_cast<size_t>(left_frames[frame_offset]->linesize[2])},
                            {video_decoder_[0]->width(), video_decoder_[0]->height()}, right_frames[frame_offset].get(), left_frames[frame_offset].get(), current_total_browsable, error_message);
        }
      }
    }
  } catch (...) {
    exception_ = std::current_exception();
  }

  frame_queue_[0]->quit();
  packet_queue_[0]->quit();
  frame_queue_[1]->quit();
  packet_queue_[1]->quit();
}
