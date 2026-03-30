#pragma once

#include <SDL2/SDL.h>

struct SDL_Renderer;
struct SDL_Window;

// Forward declarations
struct VideoCompareConfig;

class GuiManager {
 public:
  GuiManager();
  ~GuiManager();

  // Initialize ImGui with the given SDL window and renderer.
  // Must be called after SDL_CreateWindow / SDL_CreateRenderer.
  void init(SDL_Window* window, SDL_Renderer* renderer);

  // Shut down ImGui. Safe to call multiple times.
  void shutdown();

  // Process an SDL event. Returns true if ImGui consumed it
  // (i.e. mouse is over a widget or keyboard is captured by a text field).
  bool process_event(const SDL_Event& event);

  // Returns true if ImGui wants to capture mouse events (hovering over a widget).
  bool wants_capture_mouse() const;

  // Returns true if ImGui wants to capture keyboard events (text input active).
  bool wants_capture_keyboard() const;

  // Begin a new ImGui frame. Call once per iteration before any ImGui drawing.
  void begin_frame();

  // Finalize and render ImGui draw data. Call once per iteration after all ImGui drawing,
  // right before SDL_RenderPresent.
  void end_frame();

  // Returns true if ImGui has been initialized.
  bool is_initialized() const { return initialized_; }

 private:
  bool initialized_{false};
  SDL_Window* window_{nullptr};
  SDL_Renderer* renderer_{nullptr};
};
