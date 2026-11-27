#include "sdl_event_info.h"

const char* SDLEventInfo::type_name(const uint32_t type) {
  switch (type) {
    case SDL_EVENT_QUIT:
      return "SDL_EVENT_QUIT";
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      return "SDL_EVENT_WINDOW_CLOSE_REQUESTED";
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      return "SDL_EVENT_WINDOW_SIZE_CHANGED";
    case SDL_EVENT_KEY_DOWN:
      return "SDL_EVENT_KEY_DOWN";
    case SDL_EVENT_KEY_UP:
      return "SDL_EVENT_KEY_UP";
    case SDL_EVENT_TEXT_EDITING:
      return "SDL_EVENT_TEXT_EDITING";
    case SDL_EVENT_TEXT_INPUT:
      return "SDL_EVENT_TEXT_INPUT";
    case SDL_EVENT_MOUSE_MOTION:
      return "SDL_EVENT_MOUSE_MOTION";
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      return "SDL_EVENT_MOUSE_BUTTON_DOWN";
    case SDL_EVENT_MOUSE_BUTTON_UP:
      return "SDL_EVENT_MOUSE_BUTTON_UP";
    case SDL_EVENT_MOUSE_WHEEL:
      return "SDL_EVENT_MOUSE_WHEEL";
    case SDL_EVENT_DROP_FILE:
      return "SDL_EVENT_DROP_FILE";
    case SDL_EVENT_DROP_TEXT:
      return "SDL_EVENT_DROP_TEXT";
    case SDL_EVENT_DROP_BEGIN:
      return "SDL_EVENT_DROP_BEGIN";
    case SDL_EVENT_DROP_COMPLETE:
      return "SDL_EVENT_DROP_COMPLETE";
    case SDL_EVENT_RENDER_DEVICE_RESET:
      return "SDL_EVENT_RENDER_DEVICE_RESET";
    default:
      if (type >= SDL_EVENT_WINDOW_FIRST && type <= SDL_EVENT_WINDOW_LAST) {
        return "SDL_EVENT_WINDOW";
      }
      return "SDL_EVENT";
  }
}

uint32_t SDLEventInfo::window_id(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      return event.button.windowID;
    case SDL_EVENT_MOUSE_MOTION:
      return event.motion.windowID;
    case SDL_EVENT_MOUSE_WHEEL:
      return event.wheel.windowID;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      return event.key.windowID;
    case SDL_EVENT_TEXT_EDITING:
      return event.edit.windowID;
    case SDL_EVENT_TEXT_INPUT:
      return event.text.windowID;
    case SDL_EVENT_DROP_FILE:
    case SDL_EVENT_DROP_TEXT:
    case SDL_EVENT_DROP_BEGIN:
    case SDL_EVENT_DROP_COMPLETE:
      return event.drop.windowID;
    default:
      if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) {
        return event.window.windowID;
      }
      return 0;
  }
}