#pragma once

#include <algorithm>
#include <cmath>

enum class ConversionFit { Stretch, Native };

struct ContentRect {
  int x{0};
  int y{0};
  int width{0};
  int height{0};

  bool empty() const { return width <= 0 || height <= 0; }

  bool contains(const int px, const int py) const { return !empty() && px >= x && py >= y && px < (x + width) && py < (y + height); }
};

struct CanvasRect {
  int x{0};
  int y{0};
  int width{0};
  int height{0};
};

struct MappedContentRect {
  int x{0};
  int y{0};
  int width{0};
  int height{0};
  bool valid{false};
};

inline const char* conversion_fit_to_string(const ConversionFit fit) {
  switch (fit) {
    case ConversionFit::Native:
      return "native";
    case ConversionFit::Stretch:
    default:
      return "stretch";
  }
}

// Native: centered content; integer division floors, so the extra pixel on an odd
// (canvas - content) difference is on the right/bottom.
// Stretch: content fills the canvas.
inline ContentRect content_rect_in_canvas(const ConversionFit fit, const int canvas_w, const int canvas_h, const int content_w, const int content_h) {
  if (canvas_w <= 0 || canvas_h <= 0 || content_w <= 0 || content_h <= 0) {
    return {};
  }

  if (fit != ConversionFit::Native) {
    return {0, 0, canvas_w, canvas_h};
  }

  if (content_w > canvas_w || content_h > canvas_h) {
    return {};
  }

  return {(canvas_w - content_w) / 2, (canvas_h - content_h) / 2, content_w, content_h};
}

inline bool canvas_point_to_content(const ConversionFit fit, const int canvas_w, const int canvas_h, const int content_w, const int content_h, const int px, const int py, int* out_x, int* out_y) {
  if (out_x == nullptr || out_y == nullptr || canvas_w <= 0 || canvas_h <= 0 || content_w <= 0 || content_h <= 0) {
    return false;
  }

  if (fit == ConversionFit::Stretch) {
    if (px < 0 || py < 0 || px >= canvas_w || py >= canvas_h) {
      return false;
    }
    *out_x = px * content_w / canvas_w;
    *out_y = py * content_h / canvas_h;
    return true;
  }

  const ContentRect rect = content_rect_in_canvas(fit, canvas_w, canvas_h, content_w, content_h);
  if (!rect.contains(px, py)) {
    return false;
  }

  *out_x = px - rect.x;
  *out_y = py - rect.y;
  return true;
}

inline MappedContentRect canvas_selection_to_content_crop(const ConversionFit fit, const int canvas_w, const int canvas_h, const int content_w, const int content_h, const CanvasRect& selection) {
  MappedContentRect mapped;

  if (canvas_w <= 0 || canvas_h <= 0 || content_w <= 0 || content_h <= 0 || selection.width <= 0 || selection.height <= 0) {
    return mapped;
  }

  // Stretch crop in VideoCompare does not use this helper: a 1px canvas selection can
  // round to 0 here, while the existing crop path lifts width/height to kMinCropDimension.
  if (fit == ConversionFit::Stretch) {
    const auto scale = [](const int value, const int from, const int to) { return static_cast<int>(std::llround(static_cast<double>(value) * to / from)); };

    mapped.x = scale(selection.x, canvas_w, content_w);
    mapped.y = scale(selection.y, canvas_h, content_h);
    mapped.width = scale(selection.width, canvas_w, content_w);
    mapped.height = scale(selection.height, canvas_h, content_h);
  } else {
    const ContentRect content = content_rect_in_canvas(fit, canvas_w, canvas_h, content_w, content_h);
    const int x0 = std::max(selection.x, content.x);
    const int y0 = std::max(selection.y, content.y);
    const int x1 = std::min(selection.x + selection.width, content.x + content.width);
    const int y1 = std::min(selection.y + selection.height, content.y + content.height);
    if (x1 <= x0 || y1 <= y0) {
      return mapped;
    }

    mapped.x = x0 - content.x;
    mapped.y = y0 - content.y;
    mapped.width = x1 - x0;
    mapped.height = y1 - y0;
  }

  mapped.x = std::max(0, std::min(mapped.x, content_w - 1));
  mapped.y = std::max(0, std::min(mapped.y, content_h - 1));
  mapped.width = std::max(0, std::min(mapped.width, content_w - mapped.x));
  mapped.height = std::max(0, std::min(mapped.height, content_h - mapped.y));
  mapped.valid = mapped.width > 0 && mapped.height > 0;
  return mapped;
}
