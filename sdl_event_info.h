#pragma once
#include <SDL2/SDL.h>
#include <cstdint>

// Small helper for introspecting SDL events without duplicating SDL union-field knowledge everywhere.
// Kept separate from higher-level classes so `video_compare.cpp` doesn't need to carry these switch statements.
class SDLEventInfo {
 public:
  static const char* type_name(uint32_t type);
  static uint32_t window_id(const SDL_Event& event);
};
