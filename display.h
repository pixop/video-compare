#pragma once
#include "SDL2/SDL.h"
#include <array>
#include <atomic>

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
	void refresh(
		std::array<uint8_t*, 3> planes, std::array<size_t, 3> pitches);

	// Handle events
	void input();

	bool get_quit();
	bool get_play();
};
