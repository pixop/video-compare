#pragma once
#include "SDL2/SDL.h"
#include "SDL2/SDL_thread.h"
#include <atomic>
extern "C" {
	#include <libavcodec/avcodec.h>
}

class Display {
private:
	bool quit{false};
	bool play{true};

	SDL_Window* window{nullptr};
	SDL_Renderer* renderer{nullptr};
	SDL_Texture* texture{nullptr};
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
