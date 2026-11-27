#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "hud_text_cache.h"
#include "source_code_pro_regular_ttf.h"

static bool read_renderer_argb(SDL_Renderer* renderer, int width, int height, std::vector<uint32_t>& pixels) {
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

int main() {
  setenv("SDL_VIDEODRIVER", "dummy", 1);
  setenv("SDL_RENDER_DRIVER", "software", 1);
  if (!SDL_Init(SDL_INIT_VIDEO) || !TTF_Init()) {
    std::fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }
  SDL_IOStream* io1 = SDL_IOFromConstMem(SOURCE_CODE_PRO_REGULAR_TTF, SOURCE_CODE_PRO_REGULAR_TTF_LEN);
  SDL_IOStream* io2 = SDL_IOFromConstMem(SOURCE_CODE_PRO_REGULAR_TTF, SOURCE_CODE_PRO_REGULAR_TTF_LEN);
  TTF_Font* font1 = TTF_OpenFontIO(io1, true, 16.f);
  TTF_Font* font2 = TTF_OpenFontIO(io2, true, 26.f);
  SDL_Surface* canvas = SDL_CreateSurface(320, 64, SDL_PIXELFORMAT_ARGB8888);
  SDL_Renderer* renderer = canvas != nullptr ? SDL_CreateSoftwareRenderer(canvas) : nullptr;
  if (!font1 || !font2 || !renderer) {
    std::fprintf(stderr, "Font or renderer initialization failed: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }

  int comparisons = 0;
  std::vector<uint32_t> reference(320 * 64), actual(320 * 64);
  auto render = [&](SDL_Texture* texture, std::vector<uint32_t>& pixels) {
    SDL_SetRenderDrawColor(renderer, 54, 69, 79, 255);
    SDL_RenderClear(renderer);
    float w = 0.f, h = 0.f;
    SDL_GetTextureSize(texture, &w, &h);
    const SDL_FRect rect{3.f, 5.f, w, h};
    SDL_RenderTexture(renderer, texture, nullptr, &rect);
    return read_renderer_argb(renderer, 320, 64, pixels);
  };

  {
    HudTextCache cache;
    for (const auto& value : {0, 1, 2, 3, 4}) {
      TTF_Font* font = value == 4 ? font2 : font1;
      const char* str = value == 0 ? "00:00:10.000 I" : (value == 1 ? "00:00:10.040 P" : (value == 2 ? "x1.0" : (value == 3 ? "x1.0" : "x1.0")));
      SDL_Color color = value == 3 ? SDL_Color{255, 165, 0, 0} : SDL_Color{255, 255, 192, 0};
      const size_t len = std::strlen(str);
      SDL_Surface* surface = TTF_RenderText_Blended(font, str, len, color);
      SDL_Texture* standalone = SDL_CreateTextureFromSurface(renderer, surface);
      SDL_DestroySurface(surface);
      if (!standalone || !render(standalone, reference)) {
        std::fprintf(stderr, "Reference texture failed\n");
        return EXIT_FAILURE;
      }
      SDL_DestroyTexture(standalone);

      cache.update(renderer, font, str, color);
      if (!render(cache.texture(), actual) || reference != actual) {
        std::fprintf(stderr, "FAIL text pixels on case %d\n", value);
        return EXIT_FAILURE;
      }
      const SDL_Texture* stable = cache.texture();
      cache.update(renderer, font, str, color);
      if (cache.texture() != stable) {
        std::fprintf(stderr, "FAIL unchanged HUD label reallocates on case %d\n", value);
        return EXIT_FAILURE;
      }
      ++comparisons;
    }
    cache.clear();
    if (cache.texture() != nullptr) {
      std::fprintf(stderr, "FAIL cache clear\n");
      return EXIT_FAILURE;
    }
    cache.update(renderer, font1, "x1.0", SDL_Color{255, 255, 192, 0});
    if (cache.texture() == nullptr) {
      std::fprintf(stderr, "FAIL cache rebuild after reset\n");
      return EXIT_FAILURE;
    }
    cache.clear();
  }
  SDL_DestroyRenderer(renderer);
  SDL_DestroySurface(canvas);
  TTF_CloseFont(font1);
  TTF_CloseFont(font2);
  TTF_Quit();
  SDL_Quit();
  std::printf("PASS %d HUD cache pixel-equivalence and reuse cases\n", comparisons);
  return EXIT_SUCCESS;
}