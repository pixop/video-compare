#pragma once
#include "SDL2/SDL.h"
#include <atomic>
extern "C" {
	#include <libavcodec/avcodec.h>
}

class Display {
private:
	bool quit_{false};
	bool play_{true};

	SDL_Window* window_{nullptr};
	SDL_Renderer* renderer_{nullptr};
	SDL_Texture* texture_{nullptr};
	SDL_Event event_;

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
