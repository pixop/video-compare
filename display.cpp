#include "display.h"
#include <iostream>

#include <stdexcept>

using std::runtime_error;
using std::copy;

Display::Display(const unsigned width, const unsigned height) :
	quit(false),
	play(true)
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER)) {
		throw runtime_error("SDL init");
	}

	window = SDL_CreateWindow(
		"player", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		width, height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	if (!window) {
		throw runtime_error("SDL window");
	}

	renderer = SDL_CreateRenderer(
		window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!renderer) {
		throw runtime_error("SDL renderer");
	}

	texture = SDL_CreateTexture(
		renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING,
		width, height);
	if (!texture) {
		throw runtime_error("SDL texture");
	}

	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
	SDL_RenderSetLogicalSize(renderer, width, height);

	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	SDL_RenderPresent(renderer);
}

Display::~Display() {
	SDL_DestroyTexture(texture);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
}

void Display::refresh(AVFrame &frame)
{
	if (SDL_UpdateYUVTexture(
		texture, nullptr,
		frame.data[0], frame.linesize[0],
		frame.data[1], frame.linesize[1],
		frame.data[2], frame.linesize[2])) {
		throw runtime_error("SDL texture update");
	}
	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, texture, nullptr, nullptr);
	SDL_RenderPresent(renderer);
}

void Display::input()
{
	if (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_KEYUP:
			switch (event.key.keysym.sym) {
			case SDLK_ESCAPE:
				quit = true;
				break;
			case SDLK_SPACE:
				play = !play;
				break;
			default:
				break;
			}
			break;
		case SDL_QUIT:
			quit = true;
			break;
		default:
			break;
		}
	}
}

bool Display::get_quit() {
	return quit;
}

bool Display::get_play() {
	return play;
}
