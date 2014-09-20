#pragma once

#include "container.h"
#include "display.h"
#include "timer.h"
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
	std::unique_ptr<Timer> timer;

	std::unique_ptr<PacketQueue> packet_queue;
	std::unique_ptr<FrameQueue> frame_queue;

	std::vector<std::thread> stages;

	static const size_t queue_size {512 * 1024 * 1024};

public:
	Player(const std::string &file_name);
	~Player();

	void demultiplex();
	void decode_video();
	void video();

};
