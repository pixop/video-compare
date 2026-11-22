#pragma once
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace progress_dots {

// SDL used the driver line API by default before 2.0.20. Its coverage can
// differ from filled rectangles; preserve the original drawing in that case.
inline bool can_batch_for_renderer() {
  SDL_version version{};
  SDL_GetVersion(&version);
  if (SDL_VERSIONNUM(version.major, version.minor, version.patch) < SDL_VERSIONNUM(2, 0, 20)) {
    return false;
  }
  const char* method = SDL_GetHint("SDL_RENDER_LINE_METHOD");
  return method == nullptr || *method == '\0' || std::strcmp(method, "0") == 0 || std::strcmp(method, "1") == 0;
}

// Produce the same disjoint pixel strips as the legacy per-column draw loop.
// Clip to the drawable width, just as SDL would when drawing off-screen lines.
inline void build_rects(const int end_x, const int drawable_width, const int dot_width, const int dot_height, const int y,
                        std::vector<SDL_Rect>& bright, std::vector<SDL_Rect>& dark) {
  bright.clear();
  dark.clear();
  if (dot_width <= 0 || dot_height <= 0 || end_x <= 0 || drawable_width <= 0) {
    return;
  }
  const int limit = std::min(end_x, drawable_width);
  const int step = dot_width * 2;
  for (int x = 0; x < limit; x += step) {
    bright.push_back({x, y, std::min(dot_width, limit - x), dot_height});
    if (x + dot_width < limit) {
      dark.push_back({x + dot_width, y, std::min(dot_width, limit - x - dot_width), dot_height});
    }
  }
}

}  // namespace progress_dots
