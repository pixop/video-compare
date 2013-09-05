#pragma once

#include "common.h"

class Display {
	private:
		std::atomic_bool quit;
		std::atomic_bool play;
		SDL_Surface *screen;
		SDL_Overlay *bmp;
		SDL_Event event;
	public:
		Display(const unsigned width, const unsigned height);
		~Display();
		void refresh(AVFrame &frame);
		void input();
		bool get_quit();
		void set_quit();
		bool get_play();
		void toggle_play();
};
