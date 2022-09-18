#include "display.h"
#include "source_code_pro_regular_ttf.h"
#include <stdexcept>
#include <string>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <memory>
#include <libgen.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

template <typename T>
inline T check_SDL(T value, const std::string &message)
{
    if (!value)
    {
        throw std::runtime_error{"SDL " + message};
    }
    else
    {
        return value;
    }
}

inline int clampIntToByteRange(int value)
{
    return value > 255 ? 255 : value < 0 ? 0
                                         : value;
}

inline uint8_t clampIntToByte(int value)
{
    return (uint8_t)clampIntToByteRange(value);
}

// Credits to Kemin Zhou for this approach which does not require Boost or C++17
// https://stackoverflow.com/questions/4430780/how-can-i-extract-the-file-name-and-extension-from-a-path-in-c
std::string getFileStem(const std::string& filePath) {
   char* buff = new char[filePath.size() + 1];
   strcpy(buff, filePath.c_str());

   std::string tmp = std::string(basename(buff));

   const std::string::size_type i = tmp.rfind('.');

   if (i != std::string::npos) {
      tmp = tmp.substr(0, i);
   }

   delete[] buff;

   return tmp;
}

static const SDL_Color textColor = {255, 255, 255, 0};

SDL::SDL()
{
    check_SDL(!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER), "SDL init");
    check_SDL(!TTF_Init(), "TTF init");
}

SDL::~SDL()
{
    SDL_Quit();
}

Display::Display(
    const Mode mode,
    const bool high_dpi_allowed,
    const std::tuple<int, int> window_size,
    const unsigned width,
    const unsigned height,
    const std::string &left_file_name,
    const std::string &right_file_name) : mode_{mode},
                                          high_dpi_allowed_{high_dpi_allowed},
                                          video_width_{(int)width},
                                          video_height_{(int)height},
                                          left_file_stem_{getFileStem(left_file_name)},
                                          right_file_stem_{getFileStem(right_file_name)}
{
    const int window_width = std::get<0>(window_size) > 0 ? std::get<0>(window_size) : mode == Mode::hstack ? width * 2 : width;
    const int window_height = std::get<1>(window_size) > 0 ? std::get<1>(window_size) : mode == Mode::vstack ? height * 2 : height;

    const int create_window_flags = SDL_WINDOW_SHOWN;

    window_ = check_SDL(SDL_CreateWindow(
                            "video-compare", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                            high_dpi_allowed_ ? window_width / 2 : window_width, high_dpi_allowed_ ? window_height / 2 : window_height, high_dpi_allowed_ ? create_window_flags | SDL_WINDOW_ALLOW_HIGHDPI : create_window_flags),
                        "window");

    renderer_ = check_SDL(SDL_CreateRenderer(
                              window_, -1,
                              SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC),
                          "renderer");

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderPresent(renderer_);

    SDL_GL_GetDrawableSize(window_, &drawable_width_, &drawable_height_);
    SDL_GetWindowSize(window_, &window_width_, &window_height_);

    window_to_drawable_width_factor_ = (float)drawable_width_ / (float)window_width_;
    window_to_drawable_height_factor_ = (float)drawable_height_ / (float)window_height_;
    screen_to_video_width_factor_ = float(video_width_) / float(window_width_) * ((mode_ == Mode::hstack) ? 2.f : 1.f);
    screen_to_video_height_factor_ = float(video_height_) / float(window_height_) * ((mode_ == Mode::vstack) ? 2.f : 1.f);

    font_scale_ = (window_to_drawable_width_factor_ + window_to_drawable_height_factor_) / 2.0f;

    SDL_RWops *embedded_font = SDL_RWFromConstMem(source_code_pro_regular_ttf, source_code_pro_regular_ttf_len);
    small_font_ = check_SDL(TTF_OpenFontRW(embedded_font, 0, 16 * font_scale_), "font open");
    big_font_ = check_SDL(TTF_OpenFontRW(embedded_font, 0, 24 * font_scale_), "font open");

    SDL_RenderSetLogicalSize(renderer_, drawable_width_, drawable_height_);
    texture_ = check_SDL(SDL_CreateTexture(
                             renderer_, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
                             mode == Mode::hstack ? width * 2 : width, mode == Mode::vstack ? height * 2 : height),
                         "renderer");

    SDL_Surface *textSurface = TTF_RenderText_Blended(small_font_, left_file_name.c_str(), textColor);
    left_text_texture_ = SDL_CreateTextureFromSurface(renderer_, textSurface);
    left_text_width_ = textSurface->w;
    left_text_height_ = textSurface->h;
    SDL_FreeSurface(textSurface);

    textSurface = TTF_RenderText_Blended(small_font_, right_file_name.c_str(), textColor);
    right_text_texture_ = SDL_CreateTextureFromSurface(renderer_, textSurface);
    right_text_width_ = textSurface->w;
    right_text_height_ = textSurface->h;
    SDL_FreeSurface(textSurface);

    diff_buffer_ = new uint8_t[video_width_ * video_height_ * 3];
    uint8_t *diff_plane_0 = diff_buffer_;

    diff_planes_ = {diff_plane_0, NULL, NULL};
}

Display::~Display()
{
    SDL_DestroyTexture(texture_);
    SDL_DestroyTexture(left_text_texture_);
    SDL_DestroyTexture(right_text_texture_);

    if (error_message_texture_ != nullptr)
    {
        SDL_DestroyTexture(error_message_texture_);
    }

    TTF_CloseFont(small_font_);
    TTF_CloseFont(big_font_);

    delete[] diff_buffer_;

    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
}

void Display::update_difference(
    std::array<uint8_t *, 3> planes_left, std::array<size_t, 3> pitches_left,
    std::array<uint8_t *, 3> planes_right, std::array<size_t, 3> pitches_right,
    int split_x)
{
    uint8_t *p_left_r = planes_left[0] + split_x * 3;
    uint8_t *p_left_g = planes_left[0] + split_x * 3 + 1;
    uint8_t *p_left_b = planes_left[0] + split_x * 3 + 2;

    uint8_t *p_right_r = planes_right[0] + split_x * 3;
    uint8_t *p_right_g = planes_right[0] + split_x * 3 + 1;
    uint8_t *p_right_b = planes_right[0] + split_x * 3 + 2;

    uint8_t *p_diff_r = diff_planes_[0] + split_x * 3;
    uint8_t *p_diff_g = diff_planes_[0] + split_x * 3 + 1;
    uint8_t *p_diff_b = diff_planes_[0] + split_x * 3 + 2;

    const int amplification = 2;

    for (int y = 0; y < video_height_; y++)
    {
        for (int x = 0; x < (video_width_ - split_x); x++)
        {
            int rgb1[3], rgb2[3], diff[3];

            rgb1[0] = p_left_r[x * 3];
            rgb1[1] = p_left_g[x * 3];
            rgb1[2] = p_left_b[x * 3];

            rgb2[0] = p_right_r[x * 3];
            rgb2[1] = p_right_g[x * 3];
            rgb2[2] = p_right_b[x * 3];

            diff[0] = abs(rgb1[0] - rgb2[0]) * amplification;
            diff[1] = abs(rgb1[1] - rgb2[1]) * amplification;
            diff[2] = abs(rgb1[2] - rgb2[2]) * amplification;

            p_diff_r[x * 3] = clampIntToByte(diff[0]);
            p_diff_g[x * 3] = clampIntToByte(diff[1]);
            p_diff_b[x * 3] = clampIntToByte(diff[2]);
        }

        p_left_r += pitches_left[0];
        p_left_g += pitches_left[0];
        p_left_b += pitches_left[0];

        p_right_r += pitches_right[0];
        p_right_g += pitches_right[0];
        p_right_b += pitches_right[0];

        p_diff_r += video_width_ * 3;
        p_diff_g += video_width_ * 3;
        p_diff_b += video_width_ * 3;
    }
}

void Display::save_image_frames(
        std::array<uint8_t *, 3> planes_left, std::array<size_t, 3> pitches_left,
        std::array<uint8_t *, 3> planes_right, std::array<size_t, 3> pitches_right)
{
    std::ostringstream left_filename, right_filename;
    left_filename << left_file_stem_ << "_" << std::setw(4) << std::setfill('0') << saved_image_number << ".png";
    right_filename << right_file_stem_ << "_" << std::setw(4) << std::setfill('0') << saved_image_number << ".png";

    if (!stbi_write_png(left_filename.str().c_str(), video_width_, video_height_, 3, planes_left[0], pitches_left[0])) {
        std::cout << "Error saving left video PNG image to file: " << left_filename.str() << std::endl;
        return;
    } 
    if (!stbi_write_png(right_filename.str().c_str(), video_width_, video_height_, 3, planes_right[0], pitches_right[0])) {
        std::cout << "Error saving right video PNG image to file: " << left_filename.str() << std::endl;
        return;
    }

    std::cout << "Saved " << left_filename.str() << " and " << right_filename.str() << std::endl;

    saved_image_number++;
}

void Display::refresh(
    std::array<uint8_t *, 3> planes_left, std::array<size_t, 3> pitches_left,
    std::array<uint8_t *, 3> planes_right, std::array<size_t, 3> pitches_right,
    const float left_position,
    const float right_position,
    const char *current_total_browsable,
    const std::string &error_message)
{
    if (save_image_frames_) {
        save_image_frames(planes_left, pitches_left, planes_right, pitches_right);
        save_image_frames_ = false;
    }

    bool compare_mode = show_left_ && show_right_;

    int mouse_video_x = std::round(float(mouse_x_) * screen_to_video_width_factor_);
    int mouse_video_y = std::round(float(mouse_y_) * screen_to_video_height_factor_);

    // clear everything
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);

    if (show_left_ || show_right_)
    {
        int split_x = (compare_mode && mode_ == Mode::split) ? mouse_video_x : show_left_ ? video_width_ : 0;

        // update video
        if (show_left_ && (split_x > 0))
        {
            SDL_Rect tex_render_quad_left = {0, 0, split_x, video_height_};
            SDL_Rect screen_render_quad_left = video_to_screen_space(tex_render_quad_left);

            check_SDL(!SDL_UpdateTexture(texture_, &tex_render_quad_left, planes_left[0], pitches_left[0]), "left texture update (video mode)");

            SDL_RenderCopy(renderer_, texture_, &tex_render_quad_left, &screen_render_quad_left);
        }
        if (show_right_ && ((split_x < (video_width_ - 1)) || mode_ != Mode::split))
        {
            int start_right = (mode_ == Mode::split) ? split_x : 0;
            int right_x_offset = (mode_ == Mode::hstack) ? video_width_ : 0;
            int right_y_offset = (mode_ == Mode::vstack) ? video_height_ : 0;

            SDL_Rect tex_render_quad_right = {right_x_offset + start_right, right_y_offset, (video_width_ - start_right), video_height_};
            SDL_Rect screen_render_quad_right = video_to_screen_space(tex_render_quad_right);

            if (subtraction_mode_)
            {
                update_difference(planes_left, pitches_left, planes_right, pitches_right, start_right);

                check_SDL(!SDL_UpdateTexture(
                              texture_, &tex_render_quad_right,
                              diff_planes_[0] + start_right * 3, video_width_ * 3),
                          "right texture update (subtraction mode)");
            }
            else
            {
                check_SDL(!SDL_UpdateTexture(texture_, &tex_render_quad_right, planes_right[0] + start_right * 3, pitches_right[0]), "right texture update (video mode)");
            }

            SDL_RenderCopy(renderer_, texture_, &tex_render_quad_right, &screen_render_quad_right);
        }
    }

    // zoomed area
    int src_zoomed_size = 64;
    int src_half_zoomed_size = src_zoomed_size / 2;
    int dst_zoomed_size = drawable_height_ * 0.5f;
    int dst_half_zoomed_size = dst_zoomed_size / 2;

    if (zoom_left_ || zoom_right_)
    {
        SDL_Rect src_zoomed_area = {std::min(std::max(0, mouse_video_x - src_half_zoomed_size), video_width_ * ((mode_ == Mode::hstack) ? 2 : 1)), std::min(std::max(0, mouse_video_y - src_half_zoomed_size), video_height_ * ((mode_ == Mode::vstack) ? 2 : 1)), src_zoomed_size, src_zoomed_size};

        if (zoom_left_)
        {
            SDL_Rect dst_zoomed_area = {0, drawable_height_ - dst_zoomed_size, dst_zoomed_size, dst_zoomed_size};
            SDL_RenderCopy(renderer_, texture_, &src_zoomed_area, &dst_zoomed_area);
        }
        if (zoom_right_)
        {
            SDL_Rect dst_zoomed_area = {drawable_width_ - dst_zoomed_size, drawable_height_ - dst_zoomed_size, dst_zoomed_size, dst_zoomed_size};
            SDL_RenderCopy(renderer_, texture_, &src_zoomed_area, &dst_zoomed_area);
        }
    }

    SDL_Rect fill1_rect, fill2_rect;
    SDL_Rect text1_rect, text2_rect;
    SDL_Surface *textSurface;

    if (show_hud_)
    {
        // render background rectangles and text on top
        char buffer[20];
        int border_extension = 3 * font_scale_;
        int border_extension_x2 = border_extension * 2;
        int line1_y = 20;
        int line2_y = line1_y + 30 * font_scale_;

        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 64);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        if (show_left_)
        {
            // file name and current position of left video
            sprintf(buffer, "%.2f", left_position);
            textSurface = TTF_RenderText_Blended(small_font_, buffer, textColor);
            SDL_Texture *left_position_text_texture = SDL_CreateTextureFromSurface(renderer_, textSurface);
            int left_position_text_width = textSurface->w;
            int left_position_text_height = textSurface->h;
            SDL_FreeSurface(textSurface);

            fill1_rect = {line1_y - border_extension, line1_y - border_extension, left_text_width_ + border_extension_x2, left_text_height_ + border_extension_x2};
            SDL_RenderFillRect(renderer_, &fill1_rect);
            fill2_rect = {line1_y - border_extension, line2_y - border_extension, left_position_text_width + border_extension_x2, left_position_text_height + border_extension_x2};
            SDL_RenderFillRect(renderer_, &fill2_rect);
            text1_rect = {line1_y, line1_y, left_text_width_, left_text_height_};
            SDL_RenderCopy(renderer_, left_text_texture_, NULL, &text1_rect);
            text2_rect = {line1_y, line2_y, left_position_text_width, left_position_text_height};
            SDL_RenderCopy(renderer_, left_position_text_texture, NULL, &text2_rect);
            SDL_DestroyTexture(left_position_text_texture);
        }
        if (show_right_)
        {
            // file name and current position of right video
            sprintf(buffer, "%.2f", right_position);
            textSurface = TTF_RenderText_Blended(small_font_, buffer, textColor);
            SDL_Texture *right_position_text_texture = SDL_CreateTextureFromSurface(renderer_, textSurface);
            int right_position_text_width = textSurface->w;
            int right_position_text_height = textSurface->h;
            SDL_FreeSurface(textSurface);

            if (mode_ == Mode::vstack)
            {
                fill1_rect = {line1_y - border_extension, drawable_height_ - line2_y - right_text_height_ - border_extension, right_text_width_ + border_extension_x2, right_text_height_ + border_extension_x2};
                fill2_rect = {line1_y - border_extension, drawable_height_ - line1_y - right_text_height_ - border_extension, right_position_text_width + border_extension_x2, right_position_text_height + border_extension_x2};
                text1_rect = {line1_y, drawable_height_ - line2_y - right_text_height_, right_text_width_, right_text_height_};
                text2_rect = {line1_y, drawable_height_ - line1_y - right_text_height_, right_position_text_width, right_position_text_height};
            }
            else
            {
                fill1_rect = {drawable_width_ - line1_y - border_extension - right_text_width_, line1_y - border_extension, right_text_width_ + border_extension_x2, right_text_height_ + border_extension_x2};
                fill2_rect = {drawable_width_ - line1_y - border_extension - right_position_text_width, line2_y - border_extension, right_position_text_width + border_extension_x2, right_position_text_height + border_extension_x2};
                text1_rect = {drawable_width_ - line1_y - right_text_width_, line1_y, right_text_width_, right_text_height_};
                text2_rect = {drawable_width_ - line1_y - right_position_text_width, line2_y, right_position_text_width, right_position_text_height};
            }

            SDL_RenderFillRect(renderer_, &fill1_rect);
            SDL_RenderFillRect(renderer_, &fill2_rect);
            SDL_RenderCopy(renderer_, right_text_texture_, NULL, &text1_rect);
            SDL_RenderCopy(renderer_, right_position_text_texture, NULL, &text2_rect);

            SDL_DestroyTexture(right_position_text_texture);
        }

        // current frame / number of frames in history buffer
        textSurface = TTF_RenderText_Blended(small_font_, current_total_browsable, textColor);
        SDL_Texture *current_total_browsable_text_texture = SDL_CreateTextureFromSurface(renderer_, textSurface);
        int current_total_browsable_text_width = textSurface->w;
        int current_total_browsable_text_height = textSurface->h;
        SDL_FreeSurface(textSurface);

        fill1_rect = {drawable_width_ / 2 - current_total_browsable_text_width / 2 - border_extension, line1_y - border_extension, current_total_browsable_text_width + border_extension_x2, current_total_browsable_text_height + border_extension_x2};
        SDL_RenderFillRect(renderer_, &fill1_rect);
        text1_rect = {drawable_width_ / 2 - current_total_browsable_text_width / 2, line1_y, current_total_browsable_text_width, current_total_browsable_text_height};
        SDL_RenderCopy(renderer_, current_total_browsable_text_texture, NULL, &text1_rect);
        SDL_DestroyTexture(current_total_browsable_text_texture);
    }

    // render (optional) error message
    if (!error_message.empty())
    {
        error_message_shown_at_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
        textSurface = TTF_RenderText_Blended(big_font_, error_message.c_str(), textColor);
        error_message_texture_ = SDL_CreateTextureFromSurface(renderer_, textSurface);
        error_message_width_ = textSurface->w;
        error_message_height_ = textSurface->h;
        SDL_FreeSurface(textSurface);
    }
    if (error_message_texture_ != nullptr)
    {
        std::chrono::milliseconds now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
        float keep_alpha = std::max(sqrtf(1.0f - (now - error_message_shown_at_).count() / 1000.0f / 4.0f), 0.0f);

        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 64 * keep_alpha);
        fill1_rect = {drawable_width_ / 2 - error_message_width_ / 2 - 2, drawable_height_ / 2 - error_message_height_ / 2 - 2, error_message_width_ + 4, error_message_height_ + 4};
        SDL_RenderFillRect(renderer_, &fill1_rect);

        SDL_SetTextureAlphaMod(error_message_texture_, 255 * keep_alpha);
        text1_rect = {drawable_width_ / 2 - error_message_width_ / 2, drawable_height_ / 2 - error_message_height_ / 2, error_message_width_, error_message_height_};
        SDL_RenderCopy(renderer_, error_message_texture_, NULL, &text1_rect);
    }

    if (mode_ == Mode::split && show_hud_ && compare_mode)
    {
        int draw_x = std::round(float(mouse_x_) * window_to_drawable_width_factor_);

        // render movable slider(s)
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, SDL_ALPHA_OPAQUE);
        SDL_RenderDrawLine(renderer_, draw_x, 0, draw_x, drawable_height_);

        if (zoom_left_)
        {
            SDL_RenderDrawLine(renderer_, dst_half_zoomed_size, drawable_height_ - dst_zoomed_size, dst_half_zoomed_size, drawable_height_);
        }
        if (zoom_right_)
        {
            SDL_RenderDrawLine(renderer_, drawable_width_ - dst_half_zoomed_size, drawable_height_ - dst_zoomed_size, drawable_width_ - dst_half_zoomed_size, drawable_height_);
        }
    }

    SDL_RenderPresent(renderer_);
}

void Display::input()
{
    SDL_GetMouseState(&mouse_x_, &mouse_y_);

    seek_relative_ = 0.0f;
    seek_from_start_ = false;
    frame_offset_delta_ = 0;
    shift_right_frames_ = 0;

    while (SDL_PollEvent(&event_))
    {
        switch (event_.type)
        {
        case SDL_MOUSEBUTTONDOWN:
            seek_relative_ = float(mouse_x_) / float(window_width_);
            seek_from_start_ = true;
            break;
        case SDL_KEYDOWN:
            switch (event_.key.keysym.sym)
            {
            case SDLK_ESCAPE:
                quit_ = true;
                break;
            case SDLK_SPACE:
                play_ = !play_;
                break;
            case SDLK_1:
            case SDLK_KP_1:
                show_left_ = !show_left_;
                break;
            case SDLK_2:
            case SDLK_KP_2:
                show_right_ = !show_right_;
                break;
            case SDLK_3:
            case SDLK_KP_3:
                show_hud_ = !show_hud_;
                break;
            case SDLK_0:
            case SDLK_KP_0:
                subtraction_mode_ = !subtraction_mode_;
                break;
            case SDLK_z:
                zoom_left_ = true;
                break;
            case SDLK_c:
                zoom_right_ = true;
                break;
            case SDLK_a:
                frame_offset_delta_++;
                break;
            case SDLK_d:
                frame_offset_delta_--;
                break;
            case SDLK_s:
            {
                swap_left_right_ = !swap_left_right_;

                SDL_Texture *temp = left_text_texture_;
                left_text_texture_ = right_text_texture_;
                right_text_texture_ = temp;

                int temp_dim = left_text_width_;
                left_text_width_ = right_text_width_;
                right_text_width_ = temp_dim;

                temp_dim = left_text_height_;
                left_text_height_ = right_text_height_;
                right_text_height_ = temp_dim;
                break;
            }
            case SDLK_f:
                save_image_frames_ = true;
                break;
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
            case SDLK_PLUS:
            case SDLK_KP_PLUS:
                shift_right_frames_++;
                break;
            case SDLK_MINUS:
            case SDLK_KP_MINUS:
                shift_right_frames_--;
                break;
            default:
                break;
            }
            break;
        case SDL_KEYUP:
            switch (event_.key.keysym.sym)
            {
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

bool Display::get_quit()
{
    return quit_;
}

bool Display::get_play()
{
    return play_;
}

bool Display::get_swap_left_right()
{
    return swap_left_right_;
}

float Display::get_seek_relative()
{
    return seek_relative_;
}

bool Display::get_seek_from_start()
{
    return seek_from_start_;
}

int Display::get_frame_offset_delta()
{
    return frame_offset_delta_;
}

int Display::get_shift_right_frames()
{
    return shift_right_frames_;
}
