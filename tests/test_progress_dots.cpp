#include "progress_dots.h"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

// Compare pixel-for-pixel against the original per-column implementation,
// including alpha blending, clipping, and both top/bottom HUD positions.
int main(int argc, char** argv) {
  const bool accelerated = argc == 2 && std::strcmp(argv[1], "--accelerated") == 0;
  if (argc > 2 || (argc == 2 && !accelerated)) {
    std::fprintf(stderr, "Usage: test_progress_dots [--accelerated]\n");
    return EXIT_FAILURE;
  }
  if (!accelerated) {
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
  }
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }
  constexpr int width = 129;
  constexpr int height = 19;
  SDL_Window* window = nullptr;
  SDL_Surface* surface = nullptr;
  SDL_Renderer* renderer = nullptr;
  if (accelerated) {
    window = SDL_CreateWindow("Progress-dot equivalence", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, SDL_WINDOW_SHOWN);
    if (window != nullptr) {
      renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    }
  } else {
    surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface != nullptr) {
      renderer = SDL_CreateSoftwareRenderer(surface);
    }
  }
  if (renderer == nullptr) {
    std::fprintf(stderr, "SDL renderer: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_FreeSurface(surface);
    SDL_Quit();
    return EXIT_FAILURE;
  }

  std::vector<uint32_t> reference(width * height), actual(width * height);
  std::vector<SDL_Rect> bright, dark;
  const bool batching_supported = progress_dots::can_batch_for_renderer();
  if (accelerated) {
    const char* method = SDL_GetHint("SDL_RENDER_LINE_METHOD");
    std::printf("SDL line method: %s; batching enabled: %s\n", method ? method : "(default)", batching_supported ? "yes" : "no");
  }
  int cases = 0;
  for (int dot_w : {1, 2, 3, 4, 6}) {
    for (int dot_h : {1, 2, 3, 8, 20}) {
      for (int end_x : {-20, 0, 1, 2, 3, 11, 63, 128, 129, 179}) {
        for (bool is_top : {false, true}) {
          const int y = is_top ? 1 : height - 1 - dot_h;
          auto clear = [&]() {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer, 54, 69, 79, 255);
            SDL_RenderClear(renderer);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
          };
          auto read_pixels = [&](std::vector<uint32_t>& pixels) {
            return SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, pixels.data(), width * sizeof(uint32_t)) == 0;
          };
          clear();
          for (int x = 0; x < end_x; ++x) {
            if (x % (2 * dot_w) < dot_w) {
              SDL_SetRenderDrawColor(renderer, 255, 255, 192, 150);
            } else {
              SDL_SetRenderDrawColor(renderer, 0, 0, 0, 100);
            }
            SDL_RenderDrawLine(renderer, x, y, x, y + dot_h - 1);
          }
          if (!read_pixels(reference)) {
            std::fprintf(stderr, "SDL reference read failed: %s\n", SDL_GetError());
            return EXIT_FAILURE;
          }

          clear();
          if (dot_h == 1 || !batching_supported) {
            for (int x = 0; x < end_x; ++x) {
              if (x % (2 * dot_w) < dot_w) {
                SDL_SetRenderDrawColor(renderer, 255, 255, 192, 150);
              } else {
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 100);
              }
              SDL_RenderDrawLine(renderer, x, y, x, y + dot_h - 1);
            }
          } else {
            progress_dots::build_rects(end_x, width, dot_w, dot_h, y, bright, dark);
            if (!bright.empty()) {
              SDL_SetRenderDrawColor(renderer, 255, 255, 192, 150);
              SDL_RenderFillRects(renderer, bright.data(), static_cast<int>(bright.size()));
            }
            if (!dark.empty()) {
              SDL_SetRenderDrawColor(renderer, 0, 0, 0, 100);
              SDL_RenderFillRects(renderer, dark.data(), static_cast<int>(dark.size()));
            }
          }
          if (!read_pixels(actual) || reference != actual) {
            std::fprintf(stderr, "FAIL progress dots: end=%d dot=%dx%d top=%d\n", end_x, dot_w, dot_h, is_top);
            return EXIT_FAILURE;
          }
          ++cases;
        }
      }
    }
  }
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_FreeSurface(surface);
  SDL_Quit();
  std::printf("PASS %d progress-dot pixel-equivalence cases (%s renderer)\n", cases, accelerated ? "accelerated" : "software");
  return EXIT_SUCCESS;
}
