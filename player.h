#pragma once

#include "common.h"
#include "container.h"
#include "display.h"

class Player {
	private:
		std::shared_ptr<Container> container;
		std::shared_ptr<Display> display;
		std::shared_ptr<Queue<std::unique_ptr<AVPacket, void(*)(AVPacket*)>>> packet_queue;
		std::shared_ptr<Queue<std::unique_ptr<AVFrame, void(*)(AVFrame*)>>> frame_queue;
		std::vector<std::thread> stages;
	public:
		Player(const std::string &file_name);
		void demultiplex();
		void decode_video();
		void decode_audio();
		void video();
		void sound();
		void poll();
};
