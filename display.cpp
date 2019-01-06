#include "display.h"
#include <stdexcept>
#include <string>
#include <iostream>
#include <memory>

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

Display::Display(const bool high_dpi_allowed, const unsigned width, const unsigned height, const std::string &left_file_name,  const std::string &right_file_name) :
    high_dpi_allowed_{high_dpi_allowed},
    video_width_{(int) width},
    video_height_{(int) height},
    //window_width_{(int) (high_dpi_enabled_ ? width / 2 : width)},
    //window_height_{(int) (high_dpi_enabled_ ? height / 2 : height)},
	window_{check_SDL(SDL_CreateWindow(
		"video-compare", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		high_dpi_allowed_ ? width / 2 : width, high_dpi_allowed_ ? height / 2 : height, high_dpi_allowed_ ? SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI : SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE),
		"window"), SDL_DestroyWindow},
	renderer_{check_SDL(SDL_CreateRenderer(
		window_.get(), -1,
		SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC),
		"renderer"), SDL_DestroyRenderer} {
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

	SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255);
	SDL_RenderClear(renderer_.get());
	SDL_RenderPresent(renderer_.get());

    SDL_GL_GetDrawableSize(window_.get(), &drawable_width_, &drawable_height_);
    SDL_GetWindowSize(window_.get(), &window_width_, &window_height_);

    window_to_drawable_width_factor = (float) drawable_width_ / (float) window_width_;
    window_to_drawable_height_factor = (float) drawable_height_ / (float) window_height_;
    font_scale = (window_to_drawable_width_factor + window_to_drawable_height_factor) / 2.0f;

    small_font_ = check_SDL(TTF_OpenFont("SourceCodePro-Regular.ttf", 16 * font_scale), "font open");
    big_font_ = check_SDL(TTF_OpenFont("SourceCodePro-Regular.ttf", 24 * font_scale), "font open");

	SDL_RenderSetLogicalSize(renderer_.get(), drawable_width_, drawable_height_);
	texture_ = check_SDL(SDL_CreateTexture(
		renderer_.get(), SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING,
		drawable_width_, drawable_height_), "renderer");

    SDL_Surface* textSurface = TTF_RenderText_Blended(small_font_, left_file_name.c_str(), textColor);
    left_text_texture = SDL_CreateTextureFromSurface(renderer_.get(), textSurface);
    left_text_width = textSurface->w;
    left_text_height = textSurface->h;
    SDL_FreeSurface(textSurface);

    textSurface = TTF_RenderText_Blended(small_font_, right_file_name.c_str(), textColor);
    right_text_texture = SDL_CreateTextureFromSurface(renderer_.get(), textSurface);
    right_text_width = textSurface->w;
    right_text_height = textSurface->h;
    SDL_FreeSurface(textSurface);
}

Display::~Display() {
    SDL_DestroyTexture(texture_);
    SDL_DestroyTexture(left_text_texture);
    SDL_DestroyTexture(right_text_texture);
    
    if (error_message_texture != nullptr) {
        SDL_DestroyTexture(error_message_texture);
    }

    TTF_CloseFont(small_font_);
    TTF_CloseFont(big_font_);
}

void Display::refresh(
    std::array<uint8_t*, 3> planes_left, std::array<size_t, 3> pitches_left,
    std::array<uint8_t*, 3> planes_right, std::array<size_t, 3> pitches_right,
    const float left_position, 
    const float right_position, 
    const char *current_total_browsable,
    const std::string &error_message) {
    bool compare_mode = show_left_ && show_right_;

    int draw_x = mouse_x * window_to_drawable_width_factor;
    int draw_y = mouse_y * window_to_drawable_width_factor;

    // clear everything
	SDL_RenderClear(renderer_.get());

    if (show_left_ || show_right_) {
        int split_x = compare_mode ? draw_x : show_left_ ? drawable_width_ : 0;

        // update video
        if (show_left_) {
            SDL_Rect render_quad_left = { 0, 0, split_x, drawable_height_ };
                check_SDL(!SDL_UpdateYUVTexture(
                    texture_, &render_quad_left,
                    planes_left[0], pitches_left[0],
                    planes_left[1], pitches_left[1],
                    planes_left[2], pitches_left[2]), "left texture update");
        }
        if (show_right_) {
            SDL_Rect render_quad_right = { split_x, 0, (drawable_width_ - split_x), drawable_height_ };
            check_SDL(!SDL_UpdateYUVTexture(
                texture_, &render_quad_right,
                planes_right[0] + split_x, pitches_right[0],
                planes_right[1] + split_x / 2, pitches_right[1],
                planes_right[2] + split_x / 2, pitches_right[2]), "right texture update");
        }

        // render video
        SDL_RenderCopy(renderer_.get(), texture_, nullptr, nullptr);
    }

    // zoomed area
    int src_zoomed_size = 64;
    int src_half_zoomed_size = src_zoomed_size / 2;
    int dst_zoomed_size = drawable_height_ * 0.5f;
    int dst_half_zoomed_size = dst_zoomed_size / 2;

    if (zoom_left_ || zoom_right_) {
        SDL_Rect src_zoomed_area = { std::min(std::max(0, draw_x - src_half_zoomed_size), drawable_width_), std::min(std::max(0, draw_y - src_half_zoomed_size), drawable_height_), src_zoomed_size, src_zoomed_size };

        if (zoom_left_) {
            SDL_Rect dst_zoomed_area = { 0, drawable_height_ - dst_zoomed_size, dst_zoomed_size, dst_zoomed_size };
            SDL_RenderCopy(renderer_.get(), texture_, &src_zoomed_area, &dst_zoomed_area);
        }
        if (zoom_right_) {
            SDL_Rect dst_zoomed_area = { drawable_width_ - dst_zoomed_size, drawable_height_ - dst_zoomed_size, dst_zoomed_size, dst_zoomed_size };
            SDL_RenderCopy(renderer_.get(), texture_, &src_zoomed_area, &dst_zoomed_area);
        }
    }

    SDL_Rect fill_rect;
    SDL_Rect text_rect;
    SDL_Surface* textSurface;

    if (show_hud_) {
        // render background rectangles and text on top
        char buffer[20];
        int border_extension = 2;
        int border_extension_x2 = border_extension * 2;
        int line1_y = 20;
        int line2_y = line1_y + 30 * font_scale;

        SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 64);
        SDL_SetRenderDrawBlendMode(renderer_.get(), SDL_BLENDMODE_BLEND);
        if (show_left_) {
            // file name and current position of left video
            sprintf(buffer, "%.2f", left_position);
            textSurface = TTF_RenderText_Blended(small_font_, buffer, textColor);
            SDL_Texture *left_position_text_texture = SDL_CreateTextureFromSurface(renderer_.get(), textSurface);
            int left_position_text_width = textSurface->w;
            int left_position_text_height = textSurface->h;
            SDL_FreeSurface(textSurface);

            fill_rect = {line1_y - border_extension, line1_y - border_extension, left_text_width + border_extension_x2, left_text_height + border_extension_x2};
            SDL_RenderFillRect(renderer_.get(), &fill_rect);
            fill_rect = {line1_y - border_extension, line2_y - border_extension, left_position_text_width + border_extension_x2, left_position_text_height + border_extension_x2};
            SDL_RenderFillRect(renderer_.get(), &fill_rect);
            text_rect = {line1_y, line1_y, left_text_width, left_text_height};
            SDL_RenderCopy(renderer_.get(), left_text_texture, NULL, &text_rect);
            text_rect = {line1_y, line2_y, left_position_text_width, left_position_text_height};
            SDL_RenderCopy(renderer_.get(), left_position_text_texture, NULL, &text_rect);
            SDL_DestroyTexture(left_position_text_texture);
        }
        if (show_right_) {
            // file name and current position of right video
            sprintf(buffer, "%.2f", right_position);
            textSurface = TTF_RenderText_Blended(small_font_, buffer, textColor);
            SDL_Texture *right_position_text_texture = SDL_CreateTextureFromSurface(renderer_.get(), textSurface);
            int right_position_text_width = textSurface->w;
            int right_position_text_height = textSurface->h;
            SDL_FreeSurface(textSurface);
    
            fill_rect = {drawable_width_ - line1_y - border_extension - right_text_width, line1_y - border_extension, right_text_width + border_extension_x2, right_text_height + border_extension_x2};
            SDL_RenderFillRect(renderer_.get(), &fill_rect);
            fill_rect = {drawable_width_ - line1_y - border_extension - right_position_text_width, line2_y - border_extension, right_position_text_width + border_extension_x2, right_position_text_height + border_extension_x2};
            SDL_RenderFillRect(renderer_.get(), &fill_rect);
            text_rect = {drawable_width_ - line1_y - right_text_width, line1_y, right_text_width, right_text_height};
            SDL_RenderCopy(renderer_.get(), right_text_texture, NULL, &text_rect);
            text_rect = {drawable_width_ - line1_y - right_position_text_width, line2_y, right_position_text_width, right_position_text_height};
            SDL_RenderCopy(renderer_.get(), right_position_text_texture, NULL, &text_rect);
            SDL_DestroyTexture(right_position_text_texture);
        }

        // current frame / no. in history buffer
        textSurface = TTF_RenderText_Blended(small_font_, current_total_browsable, textColor);
        SDL_Texture *current_total_browsable_text_texture = SDL_CreateTextureFromSurface(renderer_.get(), textSurface);
        int current_total_browsable_text_width = textSurface->w;
        int current_total_browsable_text_height = textSurface->h;
        SDL_FreeSurface(textSurface);

        fill_rect = {drawable_width_ / 2 - current_total_browsable_text_width / 2 - border_extension, line1_y - border_extension, current_total_browsable_text_width + border_extension_x2, current_total_browsable_text_height + border_extension_x2};
        SDL_RenderFillRect(renderer_.get(), &fill_rect);
        text_rect = {drawable_width_ / 2 - current_total_browsable_text_width / 2, line1_y, current_total_browsable_text_width, current_total_browsable_text_height};
        SDL_RenderCopy(renderer_.get(), current_total_browsable_text_texture, NULL, &text_rect);
        SDL_DestroyTexture(current_total_browsable_text_texture);
    }

    // render (optional) error message
    if (!error_message.empty()) {
        error_message_shown_at = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
        textSurface = TTF_RenderText_Blended(big_font_, error_message.c_str(), textColor);
        error_message_texture = SDL_CreateTextureFromSurface(renderer_.get(), textSurface);
        error_message_width = textSurface->w;
        error_message_height = textSurface->h;
        SDL_FreeSurface(textSurface);
    }
    if (error_message_texture != nullptr) {
        std::chrono::milliseconds now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
        float keep_alpha = std::max(sqrt(1.0f - (now - error_message_shown_at).count() / 1000.0f / 4.0f), 0.0f); 

        SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 64 * keep_alpha);
        fill_rect = {drawable_width_ / 2 - error_message_width / 2 - 2, drawable_height_ / 2 - error_message_height / 2 - 2, error_message_width + 4, error_message_height + 4};
        SDL_RenderFillRect(renderer_.get(), &fill_rect);

        SDL_SetTextureAlphaMod(error_message_texture, 255 * keep_alpha);
        text_rect = {drawable_width_ / 2 - error_message_width / 2, drawable_height_ / 2 - error_message_height / 2, error_message_width, error_message_height};
        SDL_RenderCopy(renderer_.get(), error_message_texture, NULL, &text_rect);
    }

    if (show_hud_ && compare_mode) {
        // render movable slider(s)
        SDL_SetRenderDrawColor(renderer_.get(), 255, 255, 255, SDL_ALPHA_OPAQUE);
        SDL_RenderDrawLine(renderer_.get(), draw_x, 0, draw_x, drawable_height_);

        if (zoom_left_) {
            SDL_RenderDrawLine(renderer_.get(), dst_half_zoomed_size, drawable_height_ - dst_zoomed_size, dst_half_zoomed_size, drawable_height_);
        }
        if (zoom_right_) {
            SDL_RenderDrawLine(renderer_.get(), drawable_width_ - dst_half_zoomed_size, drawable_height_ - dst_zoomed_size, drawable_width_ - dst_half_zoomed_size, drawable_height_);
        }
    }

	SDL_RenderPresent(renderer_.get());
}

void Display::input() {
    SDL_GetMouseState(&mouse_x, &mouse_y);

    seek_relative_ = 0.0f;
    frame_offset_delta_ = 0;

	while (SDL_PollEvent(&event_)) {
		switch (event_.type) {
		case SDL_KEYDOWN:
			switch (event_.key.keysym.sym) {
			case SDLK_ESCAPE:
				quit_ = true;
				break;
			case SDLK_SPACE:
				play_ = !play_;
				break;
			case SDLK_1:
				show_left_ = !show_left_;
				break;
			case SDLK_2:
				show_right_ = !show_right_;
				break;
			case SDLK_3:
				show_hud_ = !show_hud_;
				break;
			case SDLK_z:
                zoom_left_ = true;
                break;
			case SDLK_c:
                zoom_right_ = true;
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
		case SDL_KEYUP:
			switch (event_.key.keysym.sym) {
			case SDLK_z:
                zoom_left_ = false;
                break;
			case SDLK_c:
                zoom_right_ = false;
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
