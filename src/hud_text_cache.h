#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdexcept>
#include <string>

// Owns one HUD texture. Unchanged labels bypass both text rasterization and
// GPU texture allocation; changed labels use the same SDL_ttf path as before.
class HudTextCache {
 public:
  HudTextCache() = default;
  HudTextCache(const HudTextCache&) = delete;
  HudTextCache& operator=(const HudTextCache&) = delete;
  ~HudTextCache() { clear(); }

  void clear() {
    if (texture_ != nullptr) {
      SDL_DestroyTexture(texture_);
      texture_ = nullptr;
    }
    font_ = nullptr;
    text_.clear();
    width_ = 0;
    height_ = 0;
    color_ = SDL_Color{};
  }

  void update(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, const SDL_Color color) {
    if (texture_ != nullptr && font_ == font && text_ == text && color_.r == color.r && color_.g == color.g && color_.b == color.b && color_.a == color.a) {
      return;
    }
    SDL_Surface* surface = TTF_RenderText_Blended(font, text.c_str(), color);
    if (surface == nullptr) {
      throw std::runtime_error(std::string("Rendering HUD text: ") + TTF_GetError());
    }
    SDL_Texture* next_texture = SDL_CreateTextureFromSurface(renderer, surface);
    const int next_width = surface->w;
    const int next_height = surface->h;
    SDL_FreeSurface(surface);
    if (next_texture == nullptr) {
      throw std::runtime_error(std::string("Creating HUD texture: ") + SDL_GetError());
    }
    SDL_DestroyTexture(texture_);
    texture_ = next_texture;
    font_ = font;
    text_ = text;
    color_ = color;
    width_ = next_width;
    height_ = next_height;
  }

  SDL_Texture* texture() const { return texture_; }
  int width() const { return width_; }
  int height() const { return height_; }

 private:
  SDL_Texture* texture_{nullptr};
  TTF_Font* font_{nullptr};  // Non-owning; caller clears the cache before closing the font.
  std::string text_;
  SDL_Color color_{};
  int width_{0};
  int height_{0};
};
