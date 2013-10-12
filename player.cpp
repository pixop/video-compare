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

Player::Player(const string &file_name) :
       	container(new Container(file_name)),
	packet_queue(new Queue<unique_ptr<AVPacket, function<void(AVPacket*)>>>(512 * 1024 * 1024)),
	frame_queue(new Queue<unique_ptr<AVFrame, function<void(AVFrame*)>>>(512 * 1024 * 1024)) {
	display.reset(new Display(container->get_width(), container->get_height()));
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
			if (display->get_quit()) {
				packet_queue->set_quit();
				break;
			}

			unique_ptr<AVPacket, function<void(AVPacket*)>> packet(new AVPacket, [](AVPacket* p){ av_free_packet(p); delete p; });
			av_init_packet(packet.get());
			packet->data = nullptr;

			if (!container->read_frame(*packet)) {
				packet_queue->set_finished();
				break; }

			if (packet->stream_index == container->get_video_stream()) {
				packet_queue->push(move(packet), packet->size);
			}
		}
	}
	catch (exception &e) {
		cerr << "Demuxing  error: " << e.what() << endl;
		exit(1);
	}

}

void Player::decode_video() {

	int finished_frame;
	const AVRational microseconds = {1, 1000000};

	try {
		for(;;) {
			if (display->get_quit()) {
				frame_queue->set_quit();
				break;
			}

			unique_ptr<AVFrame, function<void(AVFrame*)>> frame_decoded(avcodec_alloc_frame(), [](AVFrame* f){ avcodec_free_frame(&f); });
			unique_ptr<AVPacket, function<void(AVPacket*)>> packet(nullptr, [](AVPacket* p){ av_free_packet(p); delete p; });

			if (!packet_queue->pop(packet)) {
				frame_queue->set_finished();
				break;
			}

			container->decode_frame(frame_decoded, finished_frame, move(packet));

			if (finished_frame) {
				frame_decoded->pts = av_rescale_q(frame_decoded->pkt_dts, container->get_container_time_base(), microseconds);

				unique_ptr<AVFrame, function<void(AVFrame*)>> frame_converted(avcodec_alloc_frame(), [](AVFrame* f){ avpicture_free(reinterpret_cast<AVPicture*>(f)); avcodec_free_frame(&f); });
				if (av_frame_copy_props(frame_converted.get(), frame_decoded.get()) < 0) {
					throw runtime_error("Copying frame properties");
				}
				if (avpicture_alloc(reinterpret_cast<AVPicture*>(frame_converted.get()), container->get_pixel_format(), container->get_width(), container->get_height()) < 0) {
					throw runtime_error("Allocating picture"); 
				}	
				container->convert_frame(move(frame_decoded), frame_converted);
		
				frame_queue->push(move(frame_converted), 1920*1080*3);
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
		const int64_t no_lag = 0;
		int64_t frame_pts = 0;
		int64_t frame_delay = 0;
		int64_t start_time = 0;
		int64_t target_time = 0;
		int64_t lag = 0;
		int64_t display_time = 0;
		int64_t delay = 0;
		int64_t diff = 0;
		int64_t delta = 0;
		for (uint64_t frame_number = 0;; ++frame_number) {
			if (display->get_quit()) {
				break;
			}

			else if (display->get_play()) {
				unique_ptr<AVFrame, function<void(AVFrame*)>> frame(nullptr, [](AVFrame* f){ avcodec_free_frame(&f); });
				frame_queue->pop(frame);

				if (frame_number) {
					frame_delay = frame->pts - frame_pts;
					frame_pts = frame->pts;

					target_time += frame_delay;

					lag = max(no_lag, target_time - av_gettime());

					delta += diff;
					lag -= delta;

					av_usleep(lag < 0 ? 0 : static_cast<unsigned>(lag));
					//this_thread::sleep_for(chrono::microseconds(lag));

					display_time = av_gettime();
					diff = display_time - target_time;
					//cout << diff << endl;
				}
				else
				{
					frame_pts = frame->pts;
					display_time = av_gettime();
					start_time = display_time;
					target_time = display_time;
				}
				display->refresh(*frame);
			}
			else {
				milliseconds sleep(10);
				sleep_for(sleep);
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
