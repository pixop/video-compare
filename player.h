#pragma once

#include "container.h"
#include "display.h"
#include "queue.h"

#include <memory>
#include <vector>
#include <thread>
#include <string>

extern "C" {
	#include <libavcodec/avcodec.h>
}


class Player {
	private:
		std::unique_ptr<Container> container;
		std::unique_ptr<Display> display;

		std::unique_ptr<PacketQueue> packet_queue;
		std::unique_ptr<FrameQueue> frame_queue;

		std::vector<std::thread> stages;

		static const size_t queue_size;

	public:
		Player(const std::string &file_name);

		void demultiplex();
		void decode_video();
		void decode_audio();
		void video();
		void poll();

};
