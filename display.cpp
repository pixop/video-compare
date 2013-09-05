#include "display.h"

using namespace std;

Display::Display(const unsigned width, const unsigned height) {

	quit = false;
	play = true;

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTTHREAD)) {
		throw runtime_error("SDL init");
	}

	screen = SDL_SetVideoMode(width, height, 0, 0);
	if (!screen) {
		throw runtime_error("SDL video");
	}

	bmp = SDL_CreateYUVOverlay(width, height, SDL_YV12_OVERLAY, screen);
}

Display::~Display() {
	SDL_FreeYUVOverlay(bmp);
	SDL_FreeSurface(screen);
	SDL_Quit();
}

void Display::refresh(AVFrame &frame)
{
	SDL_Rect rect;

	SDL_LockYUVOverlay(bmp);

	for (size_t channel = 0; channel < 3; ++channel) {
		bmp->pitches[channel] = frame.linesize[channel];
	}

	move(&frame.data[0][0], &frame.data[0][bmp->pitches[0] * bmp->h], bmp->pixels[0]); 
	move(&frame.data[1][0], &frame.data[1][bmp->pitches[1] * bmp->h / 2], bmp->pixels[2]); 
	move(&frame.data[2][0], &frame.data[2][bmp->pitches[2] * bmp->h / 2], bmp->pixels[1]); 

	SDL_UnlockYUVOverlay(bmp);

	rect.x = 0;
	rect.y = 0;
	rect.w = bmp->w;
	rect.h = bmp->h;
	SDL_DisplayYUVOverlay(bmp, &rect);

}

void Display::input()
{
	if (SDL_WaitEvent(&event)) {
		switch (event.type) {
		case SDL_KEYUP:
			switch (event.key.keysym.sym) {
			case SDLK_ESCAPE:
				quit = true;
				break;
			case SDLK_SPACE:
				toggle_play();
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

void Display::set_quit() {
	quit = true;
}

bool Display::get_play() {
	return play;
}

void Display::toggle_play() {
	play = !play;
}
