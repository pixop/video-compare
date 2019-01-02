#include "display.h"
#include <stdexcept>
#include <string>
#include <iostream>

template <typename T>
inline T check_SDL(T value, const std::string &message) {
	if (!value) {
		throw std::runtime_error{"SDL " + message};
	} else {
		return value;
	}
}

static const SDL_Color textColor = {255, 255, 255, 0};

SDL::SDL() {
	check_SDL(!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER), "SDL init");
    check_SDL(!TTF_Init(), "TTF init");
}

SDL::~SDL() {
	SDL_Quit();
}

Display::Display(const unsigned width, const unsigned height, const std::string &left_file_name,  const std::string &right_file_name) :
    font_{check_SDL(TTF_OpenFont("SourceCodePro-Regular.ttf", 16), "font open")},
	window_{check_SDL(SDL_CreateWindow(
		"video-compare", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
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

    SDL_Surface* textSurface = TTF_RenderText_Blended(font_, left_file_name.c_str(), textColor);
    left_text_texture = SDL_CreateTextureFromSurface(renderer_.get(), textSurface);
    left_text_width = textSurface->w;
    left_text_height = textSurface->h;
    SDL_FreeSurface(textSurface);

    textSurface = TTF_RenderText_Blended(font_, right_file_name.c_str(), textColor);
    right_text_texture = SDL_CreateTextureFromSurface(renderer_.get(), textSurface);
    right_text_width = textSurface->w;
    right_text_height = textSurface->h;
    SDL_FreeSurface(textSurface);
}

Display::~Display() {
    SDL_DestroyTexture(left_text_texture);
    SDL_DestroyTexture(right_text_texture);
}

void Display::refresh(
    std::array<uint8_t*, 3> planes_left, std::array<size_t, 3> pitches_left,
    std::array<uint8_t*, 3> planes_right, std::array<size_t, 3> pitches_right,
    const int width, const int height, const float left_position, const float right_position, const char *current_total_browsable) {
    // update video
    SDL_Rect render_quad_left = { 0, 0, mouse_x, height };
	check_SDL(!SDL_UpdateYUVTexture(
		texture_.get(), &render_quad_left,
		planes_left[0], pitches_left[0],
		planes_left[1], pitches_left[1],
		planes_left[2], pitches_left[2]), "left texture update");
    SDL_Rect render_quad_right = { mouse_x, 0, width - mouse_x, height };
	check_SDL(!SDL_UpdateYUVTexture(
		texture_.get(), &render_quad_right,
		planes_right[0] + mouse_x, pitches_right[0],
		planes_right[1] + mouse_x / 2, pitches_right[1],
		planes_right[2] + mouse_x / 2, pitches_right[2]), "right texture update");

    // generate position texture
    char buffer[20];

    sprintf(buffer, "%.2f", left_position);
    SDL_Surface* textSurface = TTF_RenderText_Blended(font_, buffer, textColor);
    SDL_Texture *left_position_text_texture = SDL_CreateTextureFromSurface(renderer_.get(), textSurface);
    int left_position_text_width = textSurface->w;
    int left_position_text_height = textSurface->h;
    SDL_FreeSurface(textSurface);

    sprintf(buffer, "%.2f", right_position);
    textSurface = TTF_RenderText_Blended(font_, buffer, textColor);
    SDL_Texture *right_position_text_texture = SDL_CreateTextureFromSurface(renderer_.get(), textSurface);
    int right_position_text_width = textSurface->w;
    int right_position_text_height = textSurface->h;
    SDL_FreeSurface(textSurface);

    textSurface = TTF_RenderText_Blended(font_, current_total_browsable, textColor);
    SDL_Texture *current_total_browsable_text_texture = SDL_CreateTextureFromSurface(renderer_.get(), textSurface);
    int current_total_browsable_text_width = textSurface->w;
    int current_total_browsable_text_height = textSurface->h;
    SDL_FreeSurface(textSurface);

	SDL_RenderClear(renderer_.get());

    // render video
	SDL_RenderCopy(renderer_.get(), texture_.get(), nullptr, nullptr);

    // render background rectangles
    SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 64);
    SDL_SetRenderDrawBlendMode(renderer_.get(), SDL_BLENDMODE_BLEND);
    SDL_Rect fill_rect = {18, 18, left_text_width + 4, left_text_height + 4};
    SDL_RenderFillRect(renderer_.get(), &fill_rect);
    fill_rect = {width - 22 - right_text_width, 18, right_text_width + 4, right_text_height + 4};
    SDL_RenderFillRect(renderer_.get(), &fill_rect);
    fill_rect = {18, 48, left_position_text_width + 4, left_position_text_height + 4};
    SDL_RenderFillRect(renderer_.get(), &fill_rect);
    fill_rect = {width - 22 - right_position_text_width, 48, right_position_text_width + 4, right_position_text_height + 4};
    SDL_RenderFillRect(renderer_.get(), &fill_rect);
    fill_rect = {width / 2 - current_total_browsable_text_width / 2, 18, current_total_browsable_text_width + 4, current_total_browsable_text_height + 4};
    SDL_RenderFillRect(renderer_.get(), &fill_rect);

    // render text on top of background rectangle
    SDL_Rect text_rect = {20, 20, left_text_width, left_text_height};
    SDL_RenderCopy(renderer_.get(), left_text_texture, NULL, &text_rect);
    text_rect = {width - 20 - right_text_width, 20, right_text_width, right_text_height};
    SDL_RenderCopy(renderer_.get(), right_text_texture, NULL, &text_rect);
    text_rect = {20, 50, left_position_text_width, left_position_text_height};
    SDL_RenderCopy(renderer_.get(), left_position_text_texture, NULL, &text_rect);
    text_rect = {width - 20 - right_position_text_width, 50, right_position_text_width, right_position_text_height};
    SDL_RenderCopy(renderer_.get(), right_position_text_texture, NULL, &text_rect);
    text_rect = {width / 2 - current_total_browsable_text_width / 2, 20, current_total_browsable_text_width, current_total_browsable_text_height};
    SDL_RenderCopy(renderer_.get(), current_total_browsable_text_texture, NULL, &text_rect);

    // release memory
    SDL_DestroyTexture(left_position_text_texture);
    SDL_DestroyTexture(right_position_text_texture);
    SDL_DestroyTexture(current_total_browsable_text_texture);

    // render movable slider
    SDL_SetRenderDrawColor(renderer_.get(), 255, 255, 255, SDL_ALPHA_OPAQUE);
    SDL_RenderDrawLine(renderer_.get(), mouse_x, 0, mouse_x, height);

	SDL_RenderPresent(renderer_.get());
}

void Display::input() {
    SDL_GetMouseState(&mouse_x, &mouse_y);

    seek_relative_ = 0.0f;
    frame_offset_delta_ = 0;

	if (SDL_PollEvent(&event_)) {
		switch (event_.type) {
		case SDL_KEYDOWN:
			switch (event_.key.keysym.sym) {
			case SDLK_ESCAPE:
				quit_ = true;
				break;
			case SDLK_SPACE:
				play_ = !play_;
				break;
			case SDLK_a:
                frame_offset_delta_ += 1;
                break;
			case SDLK_d:
                frame_offset_delta_ -= 1;
                break;
			case SDLK_s:
            {
				swap_left_right_ = !swap_left_right_;

                SDL_Texture *temp = left_text_texture;
                left_text_texture = right_text_texture;
                right_text_texture = temp;

                int temp_dim = left_text_width;
                left_text_width = right_text_width;
                right_text_width = temp_dim;

                temp_dim = left_text_height;
                left_text_height = right_text_height;
                right_text_height = temp_dim;
				break;
            }
			case SDLK_LEFT:
				seek_relative_ -= 1.0f;
				break;
			case SDLK_DOWN:
				seek_relative_ -= 10.0f;
				break;
			case SDLK_PAGEDOWN:
				seek_relative_ -= 600.0f;
				break;
			case SDLK_RIGHT:
				seek_relative_ += 1.0f;
				break;
			case SDLK_UP:
				seek_relative_ += 10.0f;
				break;
			case SDLK_PAGEUP:
				seek_relative_ += 600.0f;
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

bool Display::get_swap_left_right() {
	return swap_left_right_;
}

float Display::get_seek_relative() {
	return seek_relative_;
}

int Display::get_frame_offset_delta() {
    return frame_offset_delta_;
}
