#include "player.h"
#include <algorithm>
#include <chrono>
#include <iostream>
extern "C" {
	#include <libavutil/time.h>
	#include <libavutil/imgutils.h>
}

Player::Player(const std::string &file_name) :
	container_(new Container(file_name)),
	display_(new Display(container_->get_width(), container_->get_height())),
	timer_(new Timer),
	packet_queue_(new PacketQueue(queue_size_)),
	frame_queue_(new FrameQueue(queue_size_)) {
	stages_.push_back(std::thread(&Player::demultiplex, this));
	stages_.push_back(std::thread(&Player::decode_video, this));

	video();

}

Player::~Player() {
	frame_queue_->quit();
	packet_queue_->quit();

	for (auto &stage : stages_) {
		stage.join();
	}
}

void Player::demultiplex() {
	try {
		for (;;) {
			// Create AVPacket
			std::unique_ptr<AVPacket, std::function<void(AVPacket*)>> packet(
				new AVPacket, [](AVPacket* p){ av_packet_unref(p); delete p; });
			av_init_packet(packet.get());
			packet->data = nullptr;

			// Read frame into AVPacket
			if (!container_->read_frame(*packet)) {
				packet_queue_->finished();
				break;
			}

			// Move into queue if first video stream
			if (packet->stream_index == container_->get_video_stream()) {
				if (!packet_queue_->push(move(packet))) {
					break;
				}
			}
		}
	} catch (std::exception &e) {
		std::cerr << "Demuxing error: " << e.what() << std::endl;
		exit(1);
	}
}

void Player::decode_video() {

	const AVRational microseconds = {1, 1000000};

	try {
		for (;;) {
			// Create AVFrame and AVQueue
			std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame_decoded(
				av_frame_alloc(), [](AVFrame* f){ av_frame_free(&f); });
			std::unique_ptr<AVPacket, std::function<void(AVPacket*)>> packet(
				nullptr, [](AVPacket* p){ av_packet_unref(p); delete p; });

			// Read packet from queue
			if (!packet_queue_->pop(packet)) {
				frame_queue_->finished();
				break;
			}

			// Decode packet
			int finished_frame;
			container_->decode_frame(
				frame_decoded.get(), finished_frame, packet.get());

			// If a whole frame has been decoded,
			// adjust time stamps and add to queue
			if (finished_frame) {
				frame_decoded->pts = av_rescale_q(
					frame_decoded->pkt_dts,
					container_->get_container_time_base(),
					microseconds);

				std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame_converted(
					av_frame_alloc(),
					[](AVFrame* f){ av_free(f->data[0]); });
				if (av_frame_copy_props(frame_converted.get(),
				    frame_decoded.get()) < 0) {
					throw std::runtime_error("Copying frame properties");
				}
				if (av_image_alloc(
					frame_converted->data, frame_converted->linesize,
					container_->get_width(), container_->get_height(),
					container_->get_pixel_format(), 1) < 0) {
					throw std::runtime_error("Allocating picture");
				}	
				container_->convert_frame(
					frame_decoded.get(), frame_converted.get());

				if (!frame_queue_->push(move(frame_converted))) {
					break;
				}
			}
		}
	} catch (std::exception &e) {
		std::cerr << "Decoding error: " <<  e.what() << std::endl;
		exit(1);
	}

}

void Player::video() {
	try {
		int64_t last_pts = 0;

		for (uint64_t frame_number = 0;; ++frame_number) {

			display_->input();

			if (display_->get_quit()) {
				break;

			} else if (display_->get_play()) {
				std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame(
					nullptr, [](AVFrame* f){ av_frame_free(&f); });
				if (!frame_queue_->pop(frame)) {
					break;
				}

				if (frame_number) {
					int64_t frame_delay = frame->pts - last_pts;
					last_pts = frame->pts;
					timer_->wait(frame_delay);

				} else {
					last_pts = frame->pts;
					timer_->update();
				}

				display_->refresh(*frame);

			} else {
				std::chrono::milliseconds sleep(10);
				std::this_thread::sleep_for(sleep);
				timer_->update();
			}
		}
	} catch (std::exception &e) {
		std::cerr << "Display error: " <<  e.what() << std::endl;
		exit(1);
	}
}
