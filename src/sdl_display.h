#pragma once
#include <SDL3/SDL.h>
#include <stdexcept>
#include <string>

// Map legacy 0-based display *indices* (CLI --display-number) to SDL3 display IDs.
inline SDL_DisplayID sdl_display_id_from_index(const int display_index) {
  int count = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&count);
  if (ids == nullptr) {
    throw std::runtime_error{"SDL_GetDisplays failed: " + std::string(SDL_GetError())};
  }
  if (display_index < 0 || display_index >= count) {
    SDL_free(ids);
    throw std::runtime_error{"Invalid display index " + std::to_string(display_index) + " (found " + std::to_string(count) + " displays)"};
  }
  const SDL_DisplayID id = ids[display_index];
  SDL_free(ids);
  return id;
}

inline int sdl_display_index_from_id(const SDL_DisplayID display_id) {
  if (display_id == 0) {
    return -1;
  }
  int count = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&count);
  if (ids == nullptr) {
    return -1;
  }
  int index = -1;
  for (int i = 0; i < count; ++i) {
    if (ids[i] == display_id) {
      index = i;
      break;
    }
  }
  SDL_free(ids);
  return index;
}

inline bool sdl_get_display_usable_bounds_for_index(const int display_index, SDL_Rect* rect) {
  return SDL_GetDisplayUsableBounds(sdl_display_id_from_index(display_index), rect);
}

inline bool sdl_get_display_bounds_for_index(const int display_index, SDL_Rect* rect) {
  return SDL_GetDisplayBounds(sdl_display_id_from_index(display_index), rect);
}