#pragma once

#include <atomic>

extern "C" {
	#include <libavcodec/avcodec.h>
}

#include <SDL.h>
#include <SDL_thread.h>

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

		// Copy frame to display
		void refresh(AVFrame &frame);

		// Handle events
		void input();

		bool get_quit();
		void set_quit();
		bool get_play();

};
