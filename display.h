#pragma once
#include "SDL2/SDL.h"
#include <SDL2/SDL_ttf.h>
#include <array>
#include <memory>
#include <string>
#include <chrono>

struct SDL {
	SDL();
	~SDL();
};

class Display {
private:
    bool half_mode_enabled_;
    int width_;
    int height_;

	bool quit_{false};
	bool play_{true};
    bool swap_left_right_{false};
    bool zoom_left_{false};
    bool zoom_right_{false};
    float seek_relative_{0.0f};
    int frame_offset_delta_{0};

	SDL sdl_;
    TTF_Font *small_font_;
    TTF_Font *big_font_;

    SDL_Texture *left_text_texture;
    SDL_Texture *right_text_texture;
    int left_text_width;
    int left_text_height;
    int right_text_width;
    int right_text_height;

    std::chrono::milliseconds error_message_shown_at;
    SDL_Texture *error_message_texture{nullptr};
    int error_message_width;
    int error_message_height;

	std::unique_ptr<SDL_Window, void(*)(SDL_Window*)> window_;
	std::unique_ptr<SDL_Renderer, void(*)(SDL_Renderer*)> renderer_;
	std::unique_ptr<SDL_Texture, void(*)(SDL_Texture*)> texture_;

	SDL_Event event_;
    int mouse_x;
    int mouse_y;

public:
	Display(const bool half_mode_enabled, const unsigned width, const unsigned height, const std::string &left_file_name,  const std::string &right_file_name);
    ~Display();

	// Copy frame to display
	void refresh(
		std::array<uint8_t*, 3> planes_left, std::array<size_t, 3> pitches_left,
        std::array<uint8_t*, 3> planes_right, std::array<size_t, 3> pitches_right,
        const float left_position, 
        const float right_position, 
        const char *current_total_browsable,
        const std::string &error_message);

	// Handle events
	void input();

	bool get_quit();
	bool get_play();
	bool get_swap_left_right();
    float get_seek_relative();
    int get_frame_offset_delta();
};
