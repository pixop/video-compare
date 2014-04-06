#include "player.h"

#include <chrono>
#include <iostream>
#include <algorithm>


extern "C" {
	#include <libavutil/time.h>
}


using std::string;
using std::unique_ptr;
using std::function;
using std::thread;
using std::chrono::milliseconds;
using std::this_thread::sleep_for;
using std::exception;
using std::cerr;
using std::endl;
using std::runtime_error;
using std::max;

const size_t Player::queue_size = 512 * 1048576;

Player::Player(const string &file_name) :
	container(new Container(file_name)),
	display(new Display(container->get_width(), container->get_height())),
	timer(new Timer),
	packet_queue(new Queue<unique_ptr<AVPacket, function<void(AVPacket*)>>>(
		queue_size)),
	frame_queue(new Queue<unique_ptr<AVFrame, function<void(AVFrame*)>>>(
		queue_size)) {
	stages.emplace_back(thread(&Player::demultiplex, this));
	stages.emplace_back(thread(&Player::decode_video, this));
	stages.emplace_back(thread(&Player::video, this));
	stages.emplace_back(thread(&Player::poll, this));

	while (!display->get_quit()) {
		milliseconds sleep(10);
		sleep_for(sleep);
	}

	for (auto &stage : stages) {
		stage.join();
	}	
}

void Player::demultiplex() {
	try {
		for (;;) {
			// If quitting, inform queue
			if (display->get_quit()) {
				packet_queue->set_quit();
				break;
			}

			// Create AVPacket
			unique_ptr<AVPacket, function<void(AVPacket*)>> packet(
				new AVPacket, [](AVPacket* p){ av_free_packet(p); delete p; });
			av_init_packet(packet.get());
			packet->data = nullptr;

			// Read frame into AVPacket
			if (!container->read_frame(*packet)) {
				packet_queue->set_finished();
				break;
		   	}

			// Move into queue if first video stream
			if (packet->stream_index == container->get_video_stream()) {
				packet_queue->push(move(packet), packet->size);
			}
		}

	}
	catch (exception &e) {
		cerr << "Demuxing error: " << e.what() << endl;
		exit(1);
	}
}

void Player::decode_video() {

	const AVRational microseconds = {1, 1000000};

	try {
		for(;;) {
			// If quitting, inform queues
			if (display->get_quit()) {
				frame_queue->set_quit();
				packet_queue->set_quit();
				break;
			}

			// Create AVFrame and AVQueue
			unique_ptr<AVFrame, function<void(AVFrame*)>> frame_decoded(
				av_frame_alloc(), [](AVFrame* f){ av_frame_free(&f); });
			unique_ptr<AVPacket, function<void(AVPacket*)>> packet(
				nullptr, [](AVPacket* p){ av_free_packet(p); delete p; });

			// Read packet from queue
			if (!packet_queue->pop(packet)) {
				frame_queue->set_finished();
				break;
			}

			// Decode packet
			int finished_frame;
			container->decode_frame(
				frame_decoded, finished_frame, move(packet));

			// If a whole frame has been decoded,
			// adjust time stamps and add to queue
			if (finished_frame) {
				frame_decoded->pts = av_rescale_q(
					frame_decoded->pkt_dts,
				   	container->get_container_time_base(),
				   	microseconds);

				unique_ptr<AVFrame, function<void(AVFrame*)>> frame_converted(
					av_frame_alloc(),
					[](AVFrame* f){ avpicture_free(
						reinterpret_cast<AVPicture*>(f));
						av_frame_free(&f); });
				if (av_frame_copy_props(frame_converted.get(),
				    frame_decoded.get()) < 0) {
					throw runtime_error("Copying frame properties");
				}
				if (avpicture_alloc(
					reinterpret_cast<AVPicture*>(frame_converted.get()),
				   	container->get_pixel_format(),
				   	container->get_width(), container->get_height()) < 0) {
					throw runtime_error("Allocating picture"); 
				}	
				container->convert_frame(move(frame_decoded), frame_converted);
		
				frame_queue->push(
					move(frame_converted),
					avpicture_get_size(container->get_pixel_format(),
					                   container->get_width(),
					                   container->get_height()));
			}
		}
	}
	catch (exception &e) {
		cerr << "Decoding error: " <<  e.what() << endl;
		exit(1);
	}

}

void Player::video() {
	try {
		int64_t frame_pts = 0;

		for (uint64_t frame_number = 0;; ++frame_number) {
			if (display->get_quit()) {
				break;

			} else if (display->get_play()) {
				unique_ptr<AVFrame, function<void(AVFrame*)>> frame(
					nullptr, [](AVFrame* f){ av_frame_free(&f); });
				frame_queue->pop(frame);

				int64_t frame_delay = frame->pts - frame_pts;
				frame_pts = frame->pts;

				if (frame_number) {
					timer->wait(frame_delay);

				} else {
					timer->reset();
				} 

				display->refresh(*frame);

			} else {
				milliseconds sleep(10);
				sleep_for(sleep);
				timer->reset();
			}
		}
	}
	catch (exception &e) {
		cerr << "Display error: " <<  e.what() << endl;
		exit(1);
	}
}

void Player::poll() {
	for (;;) {
		if (display->get_quit()) {
			packet_queue->set_quit();
			frame_queue->set_quit();
			break;
		}
		display->input();
	}
}
