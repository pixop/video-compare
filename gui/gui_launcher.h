#pragma once
#ifdef ENABLE_GUI

#include <string>
#include <vector>
#include "config.h"

// Forward declarations
class GuiManager;
struct SDL_Window;
struct SDL_Renderer;

class GuiLauncher {
 public:
  GuiLauncher();
  ~GuiLauncher();

  // Run the launcher event loop. Returns 0 on success, non-zero on error.
  // When the user clicks "Start comparison", this builds a VideoCompareConfig
  // and launches VideoCompare.
  int run();

 private:
  void draw_ui();
  void start_comparison();

  // SDL resources (owned by the launcher; destroyed on exit)
  SDL_Window* window_{nullptr};
  SDL_Renderer* renderer_{nullptr};
  GuiManager* gui_{nullptr};

  bool running_{true};
  bool comparison_requested_{false};

  // --- User-facing state ---
  // File paths
  char left_path_[1024]{};
  char right_path_[1024]{};

  // Filter strings
  char left_filters_[512]{};
  char right_filters_[512]{};

  // Display mode
  int display_mode_{0};  // 0=Split, 1=VStack, 2=HStack

  // Options
  bool use_10_bpc_{false};
  bool high_dpi_{false};
  bool fit_window_{false};
  bool subtraction_mode_{false};
  bool fast_alignment_{false};
  bool bilinear_texture_{false};

  // Time shift
  char time_shift_[64]{};

  // Window size
  char window_size_[32]{};

  // Auto-loop mode
  int auto_loop_mode_{0};  // 0=Off, 1=Forward, 2=PingPong

  // Frame buffer size
  int frame_buffer_size_{50};

  // Status / error message
  std::string status_message_;
  bool status_is_error_{false};
};

#endif  // ENABLE_GUI
