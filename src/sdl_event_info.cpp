#include "sdl_event_info.h"

const char* SDLEventInfo::type_name(const uint32_t type) {
  switch (type) {
    case SDL_QUIT:
      return "SDL_QUIT";
    case SDL_WINDOWEVENT:
      return "SDL_WINDOWEVENT";
    case SDL_KEYDOWN:
      return "SDL_KEYDOWN";
    case SDL_KEYUP:
      return "SDL_KEYUP";
    case SDL_TEXTEDITING:
      return "SDL_TEXTEDITING";
    case SDL_TEXTINPUT:
      return "SDL_TEXTINPUT";
    case SDL_MOUSEMOTION:
      return "SDL_MOUSEMOTION";
    case SDL_MOUSEBUTTONDOWN:
      return "SDL_MOUSEBUTTONDOWN";
    case SDL_MOUSEBUTTONUP:
      return "SDL_MOUSEBUTTONUP";
    case SDL_MOUSEWHEEL:
      return "SDL_MOUSEWHEEL";
    case SDL_DROPFILE:
      return "SDL_DROPFILE";
    case SDL_DROPTEXT:
      return "SDL_DROPTEXT";
    case SDL_DROPBEGIN:
      return "SDL_DROPBEGIN";
    case SDL_DROPCOMPLETE:
      return "SDL_DROPCOMPLETE";
    default:
      return "SDL_EVENT";
  }
}

uint32_t SDLEventInfo::window_id(const SDL_Event& event) {
  switch (event.type) {
    case SDL_WINDOWEVENT:
      return event.window.windowID;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
      return event.button.windowID;
    case SDL_MOUSEMOTION:
      return event.motion.windowID;
    case SDL_MOUSEWHEEL:
      return event.wheel.windowID;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
      return event.key.windowID;
    case SDL_TEXTEDITING:
      return event.edit.windowID;
    case SDL_TEXTINPUT:
      return event.text.windowID;
    case SDL_DROPFILE:
    case SDL_DROPTEXT:
    case SDL_DROPBEGIN:
    case SDL_DROPCOMPLETE:
      return event.drop.windowID;
    default:
      return 0;
  }
}
