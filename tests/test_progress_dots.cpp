#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "progress_dots.h"

namespace {

bool read_renderer_argb(SDL_Renderer* renderer, int width, int height, std::vector<uint32_t>& pixels) {
  SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
  if (surface == nullptr) {
    return false;
  }
  SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ARGB8888);
  SDL_DestroySurface(surface);
  if (converted == nullptr) {
    return false;
  }
  pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
  const int pitch = converted->pitch / static_cast<int>(sizeof(uint32_t));
  for (int y = 0; y < height; ++y) {
    std::memcpy(pixels.data() + y * width, static_cast<uint32_t*>(converted->pixels) + y * pitch, static_cast<size_t>(width) * sizeof(uint32_t));
  }
  SDL_DestroySurface(converted);
  return true;
}

std::vector<SDL_FRect> to_frects(const std::vector<SDL_Rect>& rects) {
  std::vector<SDL_FRect> out;
  out.reserve(rects.size());
  for (const SDL_Rect& r : rects) {
    out.push_back({static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w), static_cast<float>(r.h)});
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const bool accelerated = argc == 2 && std::strcmp(argv[1], "--accelerated") == 0;
  if (argc > 2 || (argc == 2 && !accelerated)) {
    std::fprintf(stderr, "Usage: test_progress_dots [--accelerated]\n");
    return EXIT_FAILURE;
  }
  if (!accelerated) {
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "software", SDL_HINT_OVERRIDE);
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }
  constexpr int width = 129;
  constexpr int height = 19;
  SDL_Window* window = nullptr;
  SDL_Surface* surface = nullptr;
  SDL_Renderer* renderer = nullptr;
  if (accelerated) {
    window = SDL_CreateWindow("Progress-dot equivalence", width, height, 0);
    if (window != nullptr) {
      const SDL_PropertiesID props = SDL_CreateProperties();
      SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
      renderer = SDL_CreateRendererWithProperties(props);
      SDL_DestroyProperties(props);
    }
  } else {
    surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ARGB8888);
    if (surface != nullptr) {
      renderer = SDL_CreateSoftwareRenderer(surface);
    }
  }
  if (renderer == nullptr) {
    std::fprintf(stderr, "SDL renderer: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_DestroySurface(surface);
    SDL_Quit();
    return EXIT_FAILURE;
  }

  std::vector<uint32_t> reference(width * height), actual(width * height);
  std::vector<SDL_Rect> bright, dark;
  const bool batching_supported = progress_dots::can_batch_for_renderer();
  if (accelerated) {
    std::printf("batching enabled: %s\n", batching_supported ? "yes" : "no");
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
          auto read_pixels = [&](std::vector<uint32_t>& pixels) { return read_renderer_argb(renderer, width, height, pixels); };
          clear();
          for (int x = 0; x < end_x; ++x) {
            if (x % (2 * dot_w) < dot_w) {
              SDL_SetRenderDrawColor(renderer, 255, 255, 192, 150);
            } else {
              SDL_SetRenderDrawColor(renderer, 0, 0, 0, 100);
            }
            SDL_RenderLine(renderer, static_cast<float>(x), static_cast<float>(y), static_cast<float>(x), static_cast<float>(y + dot_h - 1));
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
              SDL_RenderLine(renderer, static_cast<float>(x), static_cast<float>(y), static_cast<float>(x), static_cast<float>(y + dot_h - 1));
            }
          } else {
            progress_dots::build_rects(end_x, width, dot_w, dot_h, y, bright, dark);
            if (!bright.empty()) {
              const auto bright_f = to_frects(bright);
              SDL_SetRenderDrawColor(renderer, 255, 255, 192, 150);
              SDL_RenderFillRects(renderer, bright_f.data(), static_cast<int>(bright_f.size()));
            }
            if (!dark.empty()) {
              const auto dark_f = to_frects(dark);
              SDL_SetRenderDrawColor(renderer, 0, 0, 0, 100);
              SDL_RenderFillRects(renderer, dark_f.data(), static_cast<int>(dark_f.size()));
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
  SDL_DestroySurface(surface);
  SDL_Quit();
  std::printf("PASS %d progress-dot pixel-equivalence cases (%s renderer)\n", cases, accelerated ? "accelerated" : "software");
  return EXIT_SUCCESS;
}