#include "video_compare.h"
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

static inline bool isBehind(int64_t frame1_pts, int64_t frame2_pts) {
	float t1 = (float) frame1_pts / 1000000.0f;
	float t2 = (float) frame2_pts / 1000000.0f;

	float diff = t1 - t2;

	return diff < -(1.0f / 60.0f);
}

VideoCompare::VideoCompare(const bool high_dpi_allowed, const std::string &left_file_name, const std::string &right_file_name) :
	demuxer_{
		std::make_unique<Demuxer>(left_file_name), 
		std::make_unique<Demuxer>(right_file_name)},
	video_decoder_{
		std::make_unique<VideoDecoder>(demuxer_[0]->video_codec_parameters()), 
		std::make_unique<VideoDecoder>(demuxer_[1]->video_codec_parameters())},
	max_width_{std::max(video_decoder_[0]->width(), video_decoder_[1]->width())},
	max_height_{std::max(video_decoder_[0]->height(), video_decoder_[1]->height())},
	format_converter_{
		std::make_unique<FormatConverter>(video_decoder_[0]->width(), video_decoder_[0]->height(), max_width_, max_height_, video_decoder_[0]->pixel_format(), AV_PIX_FMT_YUV420P),
		std::make_unique<FormatConverter>(video_decoder_[1]->width(), video_decoder_[1]->height(), max_width_, max_height_, video_decoder_[1]->pixel_format(), AV_PIX_FMT_YUV420P)},
	display_{std::make_unique<Display>(high_dpi_allowed, max_width_, max_height_, left_file_name, right_file_name)},
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

void VideoCompare::decode_video(const int video_idx) {
	try {
		const AVRational microseconds = {1, 1000000};

		for (;;) {
			// Create AVFrame and AVQueue
			std::unique_ptr<AVFrame, std::function<void(AVFrame*)>>
				frame_decoded{
					av_frame_alloc(), [](AVFrame* f){ av_frame_free(&f); }};
			std::unique_ptr<AVPacket, std::function<void(AVPacket*)>> packet{
				nullptr, [](AVPacket* p){ av_packet_unref(p); delete p; }};

			// Read packet from queue
			if (!packet_queue_[video_idx]->pop(packet)) {
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
			bool sent = false;
			while (!sent && !seeking_) {
				sent = video_decoder_[video_idx]->send(packet.get());

				// If a whole frame has been decoded,
				// adjust time stamps and add to queue
				while (video_decoder_[video_idx]->receive(frame_decoded.get())) {
					frame_decoded->pts = av_rescale_q(
						frame_decoded->pkt_dts,
						demuxer_[video_idx]->time_base(),
						microseconds);

					std::unique_ptr<AVFrame, std::function<void(AVFrame*)>>
						frame_converted{
							av_frame_alloc(),
							[](AVFrame* f){ av_free(f->data[0]); }};
					if (av_frame_copy_props(frame_converted.get(),
						frame_decoded.get()) < 0) {
						throw std::runtime_error("Copying frame properties");
					}
					if (av_image_alloc(
						frame_converted->data, frame_converted->linesize,
						format_converter_[video_idx]->dest_width(), format_converter_[video_idx]->dest_height(),
						format_converter_[video_idx]->output_pixel_format(), 1) < 0) {
						throw std::runtime_error("Allocating picture");
					}
					(*format_converter_[video_idx])(
						frame_decoded.get(), frame_converted.get());

					if (!frame_queue_[video_idx]->push(move(frame_converted))) {
						break;
					}
				}
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

		std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame_left{
			nullptr, [](AVFrame* f){ av_frame_free(&f); }};
		std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame_right{
			nullptr, [](AVFrame* f){ av_frame_free(&f); }};

		int64_t left_pts = 0;
		int64_t right_pts = 0;

		for (uint64_t frame_number = 0;; ++frame_number) {
            std::string errorMessage = "";

			display_->input();

			float current_position = left_pts / 1000000.0f;

			if (display_->get_seek_relative() != 0.0f) {
                if (packet_queue_[0]->isFinished() || packet_queue_[1]->isFinished()) {
                    errorMessage = "Unable to perform seek (end of file reached)";
                } else {
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

                    bool backward = display_->get_seek_relative() < 0.0f;
                    
                    if ((!demuxer_[0]->seek(std::max(0.0f, current_position + display_->get_seek_relative()), backward) && !backward) ||
                        (!demuxer_[1]->seek(std::max(0.0f, current_position + display_->get_seek_relative()), backward) && !backward)) {
                        // restore position if unable to perform forward seek
                        errorMessage = "Unable to seek past end of file";
                        demuxer_[0]->seek(std::max(0.0f, current_position), true);
                        demuxer_[1]->seek(std::max(0.0f, current_position), true);
                    };

                    seeking_ = false;

                    frame_queue_[0]->pop(frame_left);
                    left_pts = frame_left->pts;

                    frame_queue_[1]->pop(frame_right);
                    right_pts = frame_right->pts;

                    left_frames.clear();
                    right_frames.clear();

                    current_position = frame_left->pts / 1000000.0f;
                }
			}

			bool store_frames = false;

			if (display_->get_quit()) {
				break;
			} else {
				bool adjusting = false;

				if ((left_pts < 0) || isBehind(left_pts, right_pts)) {
					adjusting = true;

					frame_queue_[0]->pop(frame_left);
				}
				if ((right_pts < 0) || isBehind(right_pts, left_pts)) {
					adjusting = true;

					frame_queue_[1]->pop(frame_right);
				}

				if (!adjusting && display_->get_play()) {
					if (!frame_queue_[0]->pop(frame_left) || !frame_queue_[1]->pop(frame_right)) {
						timer_->update();
					} else {
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
				left_pts = frame_left->pts;
			}
			if (frame_right != nullptr) {
				right_pts = frame_right->pts;
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

			if (!display_->get_swap_left_right()) {
				display_->refresh(
					{left_frames[frame_offset]->data[0], left_frames[frame_offset]->data[1], left_frames[frame_offset]->data[2]},
					{static_cast<size_t>(left_frames[frame_offset]->linesize[0]), static_cast<size_t>(left_frames[frame_offset]->linesize[1]), static_cast<size_t>(left_frames[frame_offset]->linesize[2])},
					{right_frames[frame_offset]->data[0], right_frames[frame_offset]->data[1], right_frames[frame_offset]->data[2]},
					{static_cast<size_t>(right_frames[frame_offset]->linesize[0]), static_cast<size_t>(right_frames[frame_offset]->linesize[1]), static_cast<size_t>(right_frames[frame_offset]->linesize[2])},
                    left_frames[frame_offset]->pts / 1000000.0f, 
                    right_frames[frame_offset]->pts / 1000000.0f, 
                    current_total_browsable,
                    errorMessage);
			} else {
				display_->refresh(
					{right_frames[frame_offset]->data[0], right_frames[frame_offset]->data[1], right_frames[frame_offset]->data[2]},
					{static_cast<size_t>(right_frames[frame_offset]->linesize[0]), static_cast<size_t>(right_frames[frame_offset]->linesize[1]), static_cast<size_t>(right_frames[frame_offset]->linesize[2])},
					{left_frames[frame_offset]->data[0], left_frames[frame_offset]->data[1], left_frames[frame_offset]->data[2]},
					{static_cast<size_t>(left_frames[frame_offset]->linesize[0]), static_cast<size_t>(left_frames[frame_offset]->linesize[1]), static_cast<size_t>(left_frames[frame_offset]->linesize[2])},
                    right_frames[frame_offset]->pts / 1000000.0f, 
                    left_frames[frame_offset]->pts / 1000000.0f, 
                    current_total_browsable,
                    errorMessage);
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
