#pragma once

#include <atomic>

extern "C" {
	#include <libavcodec/avcodec.h>
}

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

class Display {
private:
	bool quit;
	bool play;

	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* texture;
	SDL_Event event;

public:
	Display(const unsigned width, const unsigned height);
	~Display();

	// Copy frame to display
	void refresh(AVFrame &frame);

	// Handle events
	void input();

	bool get_quit();
	bool get_play();

};
