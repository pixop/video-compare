#pragma once

// Window size for the plain `W` shortcut: bake the current video zoom into the
// logical window so the on-screen video keeps its size and the window wraps it.
//
// Display::video_rect_to_drawable_transform() draws a full layout quad at zoom z
// across (content_window * drawable_to_window_factor * z) drawable pixels.
// SDL_SetWindowSize() takes logical window coordinates, so that extent is divided
// back by the drawable scale. Video pixels are not window pixels; the drawable
// scale cancels and must not be applied a second time after relayout.

#include <algorithm>
#include <cmath>

namespace window_fit {

// Same frame/title allowance as Display startup sizing and `-W`.
constexpr int kFrameBorderWidth = 10;
#ifdef __linux__
constexpr int kFrameBorderHeight = 40;
#else
constexpr int kFrameBorderHeight = 34;
#endif

enum class WindowSizeChord { None, FitToVideoView, RestoreSaved, RestoreStartup, SaveCurrent };

// Plain W fits. Shift / Ctrl / Ctrl+Shift keep the existing window-size commands.
// Alt and GUI (Cmd on macOS) are not those bindings. Ctrl is not Cmd.
inline WindowSizeChord window_size_chord(const bool ctrl, const bool shift, const bool alt, const bool gui) {
  if (alt || gui) {
    return WindowSizeChord::None;
  }
  if (ctrl && shift) {
    return WindowSizeChord::SaveCurrent;
  }
  if (ctrl) {
    return WindowSizeChord::RestoreStartup;
  }
  if (shift) {
    return WindowSizeChord::RestoreSaved;
  }
  return WindowSizeChord::FitToVideoView;
}

enum class FitSurfaceAction { Ignore, UnmaximizeThenFit, Fit };

// Flags are the SDL window bits. fullscreen_like is Display::detect_fullscreen_like_state().
// A borderless desktop-sized surface stays protected even if it is also maximized.
struct FitSurfaceState {
  bool native_fullscreen;
  bool borderless;
  bool maximized;
  bool fullscreen_like;
};

inline FitSurfaceAction fit_surface_action(const FitSurfaceState& state) {
  if (state.native_fullscreen) {
    return FitSurfaceAction::Ignore;
  }
  if (state.fullscreen_like && state.borderless) {
    return FitSurfaceAction::Ignore;
  }
  if (state.maximized) {
    return FitSurfaceAction::UnmaximizeThenFit;
  }
  return FitSurfaceAction::Fit;
}

struct UsableWindowLimits {
  int max_w;
  int max_h;
};

struct DisplayBounds {
  int x;
  int y;
  int w;
  int h;
};

struct WindowPosition {
  int x;
  int y;
};

// Same placement as `-W` startup sizing. `bounds` is SDL usable display bounds.
// The frame/title allowance is applied again so the window, including its
// border, sits inside that area. Integer division matches the startup code.
inline WindowPosition centered_window_position(const DisplayBounds& bounds, const int window_w, const int window_h, const int min_w, const int min_h) {
  const int usable_width = std::max(bounds.w - kFrameBorderWidth, min_w);
  const int usable_height = std::max(bounds.h - kFrameBorderHeight, min_h);

  WindowPosition position;
  position.x = bounds.x + (usable_width - window_w + kFrameBorderWidth) / 2;
  position.y = bounds.y + (usable_height - window_h + kFrameBorderHeight) / 2 + kFrameBorderWidth;
#ifdef __linux__
  position.y -= 2 * kFrameBorderWidth + 4;
#endif
  return position;
}

inline UsableWindowLimits usable_window_limits(const int display_bounds_w, const int display_bounds_h, const int min_w, const int min_h) {
  return {std::max(min_w, display_bounds_w - kFrameBorderWidth), std::max(min_h, display_bounds_h - kFrameBorderHeight)};
}

struct FitWindowInput {
  // content_window_ in SDL logical window coordinates.
  float content_w;
  float content_h;
  float zoom_factor;
  // drawable_size / window_size. Logical window size is drawable size / this.
  float drawable_to_window_width_factor;
  float drawable_to_window_height_factor;
  // Usable logical window box. Non-positive disables the cap on that call.
  int max_window_w;
  int max_window_h;
  int min_window_w;
  int min_window_h;
};

struct FitWindowResult {
  int width;
  int height;
  bool capped_to_display;
};

struct RecenteredPan {
  float move_x;
  float move_y;
  float center_x;
  float center_y;
};

// Always return to 100% after the window has absorbed the previous zoom,
// including when the target was scaled down to the usable display.
inline float zoom_factor_after_fit() {
  return 1.0F;
}

inline RecenteredPan pan_after_fit() {
  return {0.0F, 0.0F, 0.5F, 0.5F};
}

inline FitWindowResult compute_fit_window_size(const FitWindowInput& input) {
  const float zoom = std::max(input.zoom_factor, 1.0e-6F);
  const float drawable_x = std::max(input.drawable_to_window_width_factor, 1.0e-6F);
  const float drawable_y = std::max(input.drawable_to_window_height_factor, 1.0e-6F);

  // On-screen size of the full video at the current zoom, in drawable pixels.
  const float displayed_w = std::max(input.content_w, 1.0F) * drawable_x * zoom;
  const float displayed_h = std::max(input.content_h, 1.0F) * drawable_y * zoom;

  // Convert back to logical window coordinates. Do this before relayout:
  // resizing replaces content_window_ and the scale factors.
  float target_w = displayed_w / drawable_x;
  float target_h = displayed_h / drawable_y;

  bool capped = false;
  const bool limit = input.max_window_w > 0 && input.max_window_h > 0;
  if (limit && (target_w > static_cast<float>(input.max_window_w) || target_h > static_cast<float>(input.max_window_h))) {
    const float scale = std::min(static_cast<float>(input.max_window_w) / target_w, static_cast<float>(input.max_window_h) / target_h);
    target_w *= scale;
    target_h *= scale;
    capped = true;
  }

  int width = std::max(1, static_cast<int>(std::lround(target_w)));
  int height = std::max(1, static_cast<int>(std::lround(target_h)));

  if (limit) {
    const int max_w = std::max(1, input.max_window_w);
    const int max_h = std::max(1, input.max_window_h);
    if (width > max_w || height > max_h) {
      const float scale = std::min(static_cast<float>(max_w) / static_cast<float>(width), static_cast<float>(max_h) / static_cast<float>(height));
      width = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * scale)));
      height = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * scale)));
      capped = true;
    }

    const int min_w = std::min(std::max(1, input.min_window_w), max_w);
    const int min_h = std::min(std::max(1, input.min_window_h), max_h);
    width = std::max(min_w, std::min(width, max_w));
    height = std::max(min_h, std::min(height, max_h));
  } else {
    width = std::max(std::max(1, input.min_window_w), width);
    height = std::max(std::max(1, input.min_window_h), height);
  }

  return {width, height, capped};
}

}  // namespace window_fit
