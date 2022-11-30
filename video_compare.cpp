#include "video_compare.h"
#include "ffmpeg.h"
#include "sorted_flat_deque.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <deque>
extern "C" {
    #include <libavutil/time.h>
    #include <libavutil/imgutils.h>
}

const size_t VideoCompare::queue_size_{5};

static inline bool isBehind(int64_t frame1_pts, int64_t frame2_pts, int64_t delta_pts) {
    float t1 = (float) frame1_pts * AV_TIME_TO_SEC;
    float t2 = (float) frame2_pts * AV_TIME_TO_SEC;
    float delta_s = (float) delta_pts * AV_TIME_TO_SEC;

    float diff = t1 - t2;
    float tolerance = std::max(delta_s, 1.0f / 120.0f);

    return diff < -tolerance;
}

VideoCompare::VideoCompare(const Display::Mode display_mode, const bool high_dpi_allowed, const std::tuple<int, int> window_size, const double time_shift_ms, const std::string &left_file_name, const std::string &right_file_name) :
    time_shift_ms_(time_shift_ms),
    demuxer_{
        std::make_unique<Demuxer>(left_file_name),
        std::make_unique<Demuxer>(right_file_name)},
    video_decoder_{
        std::make_unique<VideoDecoder>(demuxer_[0]->video_codec_parameters()),
        std::make_unique<VideoDecoder>(demuxer_[1]->video_codec_parameters())},
    max_width_{std::max(video_decoder_[0]->width(), video_decoder_[1]->width())},
    max_height_{std::max(video_decoder_[0]->height(), video_decoder_[1]->height())},
    format_converter_{
        std::make_unique<FormatConverter>(video_decoder_[0]->width(), video_decoder_[0]->height(), max_width_, max_height_, video_decoder_[0]->pixel_format(), AV_PIX_FMT_RGB24),
        std::make_unique<FormatConverter>(video_decoder_[1]->width(), video_decoder_[1]->height(), max_width_, max_height_, video_decoder_[1]->pixel_format(), AV_PIX_FMT_RGB24)},
    display_{std::make_unique<Display>(display_mode, high_dpi_allowed, window_size, max_width_, max_height_, left_file_name, right_file_name)},
    timer_{std::make_unique<Timer>()},
    packet_queue_{
        std::make_unique<PacketQueue>(queue_size_),
        std::make_unique<PacketQueue>(queue_size_)},
    frame_queue_{
        std::make_unique<FrameQueue>(queue_size_),
        std::make_unique<FrameQueue>(queue_size_)} {
}

void VideoCompare::operator()() {
    stages_.emplace_back(&VideoCompare::thread_demultiplex_left, this);
    stages_.emplace_back(&VideoCompare::thread_demultiplex_right, this);
    stages_.emplace_back(&VideoCompare::thread_decode_video_left, this);
    stages_.emplace_back(&VideoCompare::thread_decode_video_right, this);
    video();

    for (auto &stage : stages_) {
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
            std::unique_ptr<AVPacket, std::function<void(AVPacket*)>> packet{
                new AVPacket,
                [](AVPacket* p){ av_packet_unref(p); delete p; }};
            av_init_packet(packet.get());
            packet->data = nullptr;

            // Read frame into AVPacket
            if (!(*demuxer_[video_idx])(*packet)) {
                packet_queue_[video_idx]->finished();
                break;
            }

            // Move into queue if first video stream
            if (packet->stream_index == demuxer_[video_idx]->video_stream_index()) {
                if (!packet_queue_[video_idx]->push(move(packet))) {
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

bool VideoCompare::process_packet(const int video_idx, AVPacket *packet, AVFrame *frame_decoded) {
    const AVRational microseconds = {1, AV_TIME_BASE};

    bool sent = video_decoder_[video_idx]->send(packet);

    // If a whole frame has been decoded,
    // adjust time stamps and add to queue
    while (video_decoder_[video_idx]->receive(frame_decoded)) {
        frame_decoded->pts = av_rescale_q(
            frame_decoded->pts,
            demuxer_[video_idx]->time_base(),
            microseconds);

        std::unique_ptr<AVFrame, std::function<void(AVFrame*)>>
            frame_converted{ av_frame_alloc(), [](AVFrame* f){ av_free(f->data[0]); }};

        if (av_frame_copy_props(frame_converted.get(),
            frame_decoded) < 0) {
            throw std::runtime_error("Copying frame properties");
        }
        if (av_image_alloc(
            frame_converted->data, frame_converted->linesize,
            format_converter_[video_idx]->dest_width(), format_converter_[video_idx]->dest_height(),
            format_converter_[video_idx]->output_pixel_format(), 1) < 0) {
            throw std::runtime_error("Allocating picture");
        }
        (*format_converter_[video_idx])(
            frame_decoded, frame_converted.get());

        if (!frame_queue_[video_idx]->push(move(frame_converted))) {
            break;
        }
    }

    return sent;
}

void VideoCompare::decode_video(const int video_idx) {
    try {
        AVPacket *prev_packet = nullptr;

        for (;;) {
            // Create AVFrame and AVQueue
            std::unique_ptr<AVFrame, std::function<void(AVFrame*)>>
                frame_decoded{ av_frame_alloc(), [](AVFrame* f){ av_frame_free(&f); }};
            std::unique_ptr<AVPacket, std::function<void(AVPacket*)>>
                packet{ nullptr, [](AVPacket* p){ av_packet_unref(p); delete p; }};

            // Read packet from queue
            if (!packet_queue_[video_idx]->pop(packet)) {
                if (prev_packet != nullptr) {
                    // Decode frames cached in the decoder
                    while (process_packet(video_idx, prev_packet, frame_decoded.get()));
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
            while (!process_packet(video_idx, packet.get(), frame_decoded.get()) && !seeking_);

            prev_packet = packet.get();
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

        std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame_left{
            nullptr, [](AVFrame* f){ av_frame_free(&f); }};
        std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame_right{
            nullptr, [](AVFrame* f){ av_frame_free(&f); }};

        int64_t left_pts = 0, left_decoded_picture_number = 0, left_previous_decoded_picture_number = -1, delta_left_pts = 0;
        int64_t right_pts = 0, right_decoded_picture_number = 0, right_previous_decoded_picture_number = -1, delta_right_pts = 0;

        sorted_flat_deque<int32_t> left_deque(8), right_deque(8);

        int64_t right_time_shift = time_shift_ms_ * MILLISEC_TO_AV_TIME;
        int total_right_time_shifted = 0;

        for (uint64_t frame_number = 0;; ++frame_number) {
            std::string errorMessage = "";

            display_->input();

            float current_position = left_pts * AV_TIME_TO_SEC;

            if ((display_->get_seek_relative() != 0.0f) || (display_->get_shift_right_frames() != 0)) {
                total_right_time_shifted += display_->get_shift_right_frames();

                if (packet_queue_[0]->isFinished() || packet_queue_[1]->isFinished()) {
                    errorMessage = "Unable to perform seek (end of file reached)";
                } else {
                    // compute effective time shift and round down to nearest 2 ms
                    right_time_shift = time_shift_ms_ * MILLISEC_TO_AV_TIME + total_right_time_shifted * (delta_right_pts > 0 ? delta_right_pts : 10000);
                    right_time_shift = ((right_time_shift / 1000) - 2) * 1000;

                    seeking_ = true;
                    readyToSeek_[0][0] = false;
                    readyToSeek_[0][1] = false;
                    readyToSeek_[1][0] = false;
                    readyToSeek_[1][1] = false;

                    while(true) {
                        bool allEmpty = true;
                        
                        allEmpty = allEmpty && readyToSeek_[0][0];
                        allEmpty = allEmpty && readyToSeek_[0][1];
                        allEmpty = allEmpty && readyToSeek_[1][0];
                        allEmpty = allEmpty && readyToSeek_[1][1];

                        if (allEmpty) {
                            break;
                        } else {
                            frame_queue_[0]->empty();
                            frame_queue_[1]->empty();
                        }
                    }

                    packet_queue_[0]->empty();
                    packet_queue_[1]->empty();
                    frame_queue_[0]->empty();
                    frame_queue_[1]->empty();

                    float next_position;

                    if (display_->get_seek_from_start()) {
                        // seek from start based on first stream duration in seconds
                        next_position = (demuxer_[0]->duration() * AV_TIME_TO_SEC * display_->get_seek_relative());
                    } else {
                        next_position = current_position + display_->get_seek_relative();
                    }
                    
                    bool backward = (display_->get_seek_relative() < 0.0f) || (display_->get_shift_right_frames() != 0);

                    if ((!demuxer_[0]->seek(std::max(0.0f, next_position), backward) && !backward) ||
                        (!demuxer_[1]->seek(std::max(0.0f, next_position), backward) && !backward)) {
                        // restore position if unable to perform forward seek
                        errorMessage = "Unable to seek past end of file";
                        demuxer_[0]->seek(std::max(0.0f, current_position), true);
                        demuxer_[1]->seek(std::max(0.0f, current_position), true);
                    };

                    seeking_ = false;

                    frame_queue_[0]->pop(frame_left);
                    left_pts = frame_left->pts;
                    left_previous_decoded_picture_number = -1;
                    left_decoded_picture_number = 1;

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

            if (display_->get_quit()) {
                break;
            } else {
                bool adjusting = false;

                // use the delta between current and previous PTS as the tolerance which determines whether we have to adjust
                if ((left_pts < 0) || isBehind(left_pts, right_pts, delta_left_pts)) {
                    adjusting = true;

                    if (frame_queue_[0]->pop(frame_left)) {
                        left_decoded_picture_number++;
                    }
                }
                if ((right_pts < 0) || isBehind(right_pts, left_pts, delta_right_pts)) {
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
                // TODO: use pair
                if (left_frames.size() >= 50) {
                    left_frames.pop_back();
                }
                if (right_frames.size() >= 50) {
                    right_frames.pop_back();
                }

                left_frames.push_front(move(frame_left));
                right_frames.push_front(move(frame_right));
            } else {
                if (frame_left != nullptr) {
                    if (left_frames.size() > 0) {
                        left_frames[0] = move(frame_left);
                    } else {
                        left_frames.push_front(move(frame_left));
                    }
                }
                if (frame_right != nullptr) {
                    if (right_frames.size() > 0) {
                        right_frames[0] = move(frame_right);
                    } else {
                        right_frames.push_front(move(frame_right));
                    }
                }
            }

            frame_offset = std::min(std::max(0, frame_offset + display_->get_frame_offset_delta()), (int) left_frames.size() - 1);

            char current_total_browsable[20];
            sprintf(current_total_browsable, "%d/%d", frame_offset + 1, (int) left_frames.size());

            if (frame_offset >= 0) {
                if (!display_->get_swap_left_right()) {
                    display_->refresh(
                        {left_frames[frame_offset]->data[0], left_frames[frame_offset]->data[1], left_frames[frame_offset]->data[2]},
                        {static_cast<size_t>(left_frames[frame_offset]->linesize[0]), static_cast<size_t>(left_frames[frame_offset]->linesize[1]), static_cast<size_t>(left_frames[frame_offset]->linesize[2])},
                        {right_frames[frame_offset]->data[0], right_frames[frame_offset]->data[1], right_frames[frame_offset]->data[2]},
                        {static_cast<size_t>(right_frames[frame_offset]->linesize[0]), static_cast<size_t>(right_frames[frame_offset]->linesize[1]), static_cast<size_t>(right_frames[frame_offset]->linesize[2])},
                        left_frames[frame_offset]->pts * AV_TIME_TO_SEC,
                        right_frames[frame_offset]->pts * AV_TIME_TO_SEC,
                        current_total_browsable,
                        errorMessage);
                } else {
                    display_->refresh(
                        {right_frames[frame_offset]->data[0], right_frames[frame_offset]->data[1], right_frames[frame_offset]->data[2]},
                        {static_cast<size_t>(right_frames[frame_offset]->linesize[0]), static_cast<size_t>(right_frames[frame_offset]->linesize[1]), static_cast<size_t>(right_frames[frame_offset]->linesize[2])},
                        {left_frames[frame_offset]->data[0], left_frames[frame_offset]->data[1], left_frames[frame_offset]->data[2]},
                        {static_cast<size_t>(left_frames[frame_offset]->linesize[0]), static_cast<size_t>(left_frames[frame_offset]->linesize[1]), static_cast<size_t>(left_frames[frame_offset]->linesize[2])},
                        right_frames[frame_offset]->pts * AV_TIME_TO_SEC,
                        left_frames[frame_offset]->pts * AV_TIME_TO_SEC,
                        current_total_browsable,
                        errorMessage);
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
