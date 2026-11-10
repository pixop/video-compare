#pragma once

// Viewport zoom and physical drawable-pixel scale for Shift+X diagnostics.
// Mirrors Display::video_rect_to_drawable_transform() after video_to_zoom_space():
// one layout video pixel spans (global_zoom * drawable_to_window / video_to_window)
// drawable pixels on each axis.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace display_pixel_scale {

struct PhysicalPixelScale {
  float x;
  float y;
};

inline float safe_positive(const float value) {
  return std::max(value, 1.0e-6F);
}

inline PhysicalPixelScale compute_physical_pixel_scale(const float viewport_zoom, const float drawable_to_window_width_factor, const float drawable_to_window_height_factor,
                                                       const float video_to_window_width_factor, const float video_to_window_height_factor) {
  const float zoom = std::max(viewport_zoom, 0.0F);
  const float drawable_x = safe_positive(drawable_to_window_width_factor);
  const float drawable_y = safe_positive(drawable_to_window_height_factor);
  const float video_x = safe_positive(video_to_window_width_factor);
  const float video_y = safe_positive(video_to_window_height_factor);

  return {zoom * drawable_x / video_x, zoom * drawable_y / video_y};
}

inline bool scales_nearly_equal(const float a, const float b) {
  const float diff = std::fabs(a - b);
  const float scale = std::max(std::fabs(a), std::fabs(b));
  const float tolerance = std::max(1.0e-4F, 1.0e-3F * scale);
  return diff <= tolerance;
}

// HUD-aligned viewport zoom text (e.g. x1, x2).
inline std::string format_viewport_zoom_factor(const float global_zoom_factor) {
  std::string zoom_factor_str;
  const uint64_t global_zoom_factor_rounded = static_cast<uint64_t>(std::lrintf(global_zoom_factor * 1000.0F));
  int global_zoom_factor_trailing_zeros = (global_zoom_factor_rounded % 10) > 0 ? 0 : 1;
  global_zoom_factor_trailing_zeros += (global_zoom_factor_rounded % 100) > 0 ? 0 : 1;
  global_zoom_factor_trailing_zeros += (global_zoom_factor_rounded % 1000) > 0 ? 0 : 1;

  char buf[32];
  if (global_zoom_factor < 1e-1F || (global_zoom_factor_trailing_zeros == 0 && global_zoom_factor_rounded < 1000)) {
    std::snprintf(buf, sizeof(buf), "x%1.3f", global_zoom_factor);
  } else if (global_zoom_factor_trailing_zeros <= 1 && global_zoom_factor_rounded < 10000) {
    std::snprintf(buf, sizeof(buf), "x%1.2f", global_zoom_factor);
  } else if (global_zoom_factor_trailing_zeros <= 2 && global_zoom_factor_rounded < 100000) {
    std::snprintf(buf, sizeof(buf), "x%1.1f", global_zoom_factor);
  } else {
    std::snprintf(buf, sizeof(buf), "x%1.0f", global_zoom_factor);
  }
  zoom_factor_str = buf;
  return zoom_factor_str;
}

inline std::string format_pixel_scale_percent(const float scale) {
  char buf[32];
  const float pct = scale * 100.0F;
  const int as_int = static_cast<int>(std::lrintf(pct));
  if (std::fabs(pct - static_cast<float>(as_int)) <= 0.05F) {
    std::snprintf(buf, sizeof(buf), "%d%%", as_int);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f%%", pct);
  }
  return buf;
}

inline void append_display_state_viewport_and_pixel_scale(std::string& message, const float global_zoom_factor, const float drawable_to_window_width_factor, const float drawable_to_window_height_factor,
                                                          const float video_to_window_width_factor, const float video_to_window_height_factor) {
  message += " zoom=" + format_viewport_zoom_factor(global_zoom_factor);

  const PhysicalPixelScale scale = compute_physical_pixel_scale(global_zoom_factor, drawable_to_window_width_factor, drawable_to_window_height_factor, video_to_window_width_factor,
                                                               video_to_window_height_factor);
  if (scales_nearly_equal(scale.x, scale.y)) {
    message += " pixel_scale=" + format_pixel_scale_percent(scale.x);
  } else {
    message += " pixel_scale_x=" + format_pixel_scale_percent(scale.x);
    message += " pixel_scale_y=" + format_pixel_scale_percent(scale.y);
  }
}

}  // namespace display_pixel_scale
