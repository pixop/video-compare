#include "display.h"
#include <stdexcept>

template <typename T>
inline T check_SDL(T value, const std::string &message) {
	if (!value) {
		throw std::runtime_error{"SDL " + message};
	} else {
		return value;
	}
}

SDL::SDL() {
	check_SDL(!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER), "init");
}

SDL::~SDL() {
	SDL_Quit();
}

Display::Display(const unsigned width, const unsigned height) :
	window_{check_SDL(SDL_CreateWindow(
		"player", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		width, height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE),
		"window"), SDL_DestroyWindow},
	renderer_{check_SDL(SDL_CreateRenderer(
		window_.get(), -1,
		SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC),
		"renderer"), SDL_DestroyRenderer},
	texture_{check_SDL(SDL_CreateTexture(
		renderer_.get(), SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING,
		width, height), "renderer"), SDL_DestroyTexture} {

	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
	SDL_RenderSetLogicalSize(renderer_.get(), width, height);

	SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255);
	SDL_RenderClear(renderer_.get());
	SDL_RenderPresent(renderer_.get());
}

void Display::refresh(
	std::array<uint8_t*, 3> planes, std::array<size_t, 3> pitches) {
	check_SDL(!SDL_UpdateYUVTexture(
		texture_.get(), nullptr,
		planes[0], pitches[0],
		planes[1], pitches[1],
		planes[2], pitches[2]), "texture update");
	SDL_RenderClear(renderer_.get());
	SDL_RenderCopy(renderer_.get(), texture_.get(), nullptr, nullptr);
	SDL_RenderPresent(renderer_.get());
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
