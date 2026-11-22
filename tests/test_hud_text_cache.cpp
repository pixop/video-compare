#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "hud_text_cache.h"
#include "source_code_pro_regular_ttf.h"

int main() {
  SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
  if (SDL_Init(SDL_INIT_VIDEO) != 0 || TTF_Init() != 0) {
    std::fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }
  SDL_RWops* rw1 = SDL_RWFromConstMem(SOURCE_CODE_PRO_REGULAR_TTF, SOURCE_CODE_PRO_REGULAR_TTF_LEN);
  SDL_RWops* rw2 = SDL_RWFromConstMem(SOURCE_CODE_PRO_REGULAR_TTF, SOURCE_CODE_PRO_REGULAR_TTF_LEN);
  TTF_Font* font1 = TTF_OpenFontRW(rw1, 1, 16);
  TTF_Font* font2 = TTF_OpenFontRW(rw2, 1, 26);
  SDL_Surface* canvas = SDL_CreateRGBSurfaceWithFormat(0, 320, 64, 32, SDL_PIXELFORMAT_ARGB8888);
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
    int w = 0, h = 0;
    SDL_QueryTexture(texture, nullptr, nullptr, &w, &h);
    const SDL_Rect rect{3, 5, w, h};
    SDL_RenderCopy(renderer, texture, nullptr, &rect);
    return SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, pixels.data(), 320 * sizeof(uint32_t)) == 0;
  };

  {
    HudTextCache cache;
    for (const auto& value : {0, 1, 2, 3, 4}) {
      TTF_Font* font = value == 4 ? font2 : font1;
      const char* str = value == 0 ? "00:00:10.000 I" : (value == 1 ? "00:00:10.040 P" : (value == 2 ? "x1.0" : (value == 3 ? "x1.0" : "x1.0")));
      SDL_Color color = value == 3 ? SDL_Color{255, 165, 0, 0} : SDL_Color{255, 255, 192, 0};
      SDL_Surface* surface = TTF_RenderText_Blended(font, str, color);
      SDL_Texture* standalone = SDL_CreateTextureFromSurface(renderer, surface);
      SDL_FreeSurface(surface);
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
    // A renderer-device reset invalidates the cache; the next use should
    // recreate the texture without requiring a font or label change.
    cache.update(renderer, font1, "x1.0", SDL_Color{255, 255, 192, 0});
    if (cache.texture() == nullptr) {
      std::fprintf(stderr, "FAIL cache rebuild after reset\n");
      return EXIT_FAILURE;
    }
    cache.clear();
  }
  SDL_DestroyRenderer(renderer);
  SDL_FreeSurface(canvas);
  TTF_CloseFont(font1);
  TTF_CloseFont(font2);
  TTF_Quit();
  SDL_Quit();
  std::printf("PASS %d HUD cache pixel-equivalence and reuse cases\n", comparisons);
  return EXIT_SUCCESS;
}
