#include "display.h"
#include <iostream>
#include <stdexcept>

Display::Display(const unsigned width, const unsigned height) {
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER)) {
		throw std::runtime_error("SDL init");
	}

	window_ = SDL_CreateWindow(
		"player", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		width, height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	if (!window_) {
		throw std::runtime_error("SDL window");
	}

	renderer_ = SDL_CreateRenderer(
		window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!renderer_) {
		throw std::runtime_error("SDL renderer");
	}

	texture_ = SDL_CreateTexture(
		renderer_, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING,
		width, height);
	if (!texture_) {
		throw std::runtime_error("SDL texture");
	}

	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
	SDL_RenderSetLogicalSize(renderer_, width, height);

	SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
	SDL_RenderClear(renderer_);
	SDL_RenderPresent(renderer_);
}

Display::~Display() {
	SDL_DestroyTexture(texture_);
	SDL_DestroyRenderer(renderer_);
	SDL_DestroyWindow(window_);
	SDL_Quit();
}

void Display::refresh(
	std::array<uint8_t*, 3> planes, std::array<size_t, 3> pitches) {
	if (SDL_UpdateYUVTexture(
		texture_, nullptr,
		planes[0], pitches[0],
		planes[1], pitches[1],
		planes[2], pitches[2])) {
		throw std::runtime_error("SDL texture update");
	}
	SDL_RenderClear(renderer_);
	SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
	SDL_RenderPresent(renderer_);
}

void Display::input() {
	if (SDL_PollEvent(&event_)) {
		switch (event_.type) {
		case SDL_KEYUP:
			switch (event_.key.keysym.sym) {
			case SDLK_ESCAPE:
				quit_ = true;
				break;
			case SDLK_SPACE:
				play_ = !play_;
				break;
			default:
				break;
			}
			break;
		case SDL_QUIT:
			quit_ = true;
			break;
		default:
			break;
		}
	}
}

bool Display::get_quit() {
	return quit_;
}

bool Display::get_play() {
	return play_;
}
