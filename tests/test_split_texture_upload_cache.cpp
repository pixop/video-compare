#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "split_texture_upload_cache.h"

namespace {

constexpr int kWidth = 133;
constexpr int kHeight = 47;
constexpr int kPitch = kWidth * 3;
// Mirror the aligned RGB48LE linesize used as the packed output pitch.
constexpr int kPitch10 = ((kWidth * 6 + 63) / 64) * 64;

struct Frame {
  int id;
  int generation = 0;

  std::string key() const { return std::to_string(generation) + ":0"; }

  std::vector<uint8_t> pixels(bool ten_bpc) const {
    const int pitch = ten_bpc ? kPitch10 : kPitch;
    std::vector<uint8_t> pixels(pitch * kHeight);
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        const int r = (id * 41 + generation * 17 + x * 3 + y) & (ten_bpc ? 1023 : 255);
        const int g = (id * 53 + generation * 19 + x + y * 7) & (ten_bpc ? 1023 : 255);
        const int b = (id * 67 + generation * 23 + x * 5 + y * 3) & (ten_bpc ? 1023 : 255);
        if (ten_bpc) {
          // The display path packs 16-bit RGB48LE (10 valid bits stored
          // high) to 32-bit ARGB2101010 using the source frame's pitch.
          const uint16_t rgb48[] = {static_cast<uint16_t>(r << 6), static_cast<uint16_t>(g << 6), static_cast<uint16_t>(b << 6)};
          const uint32_t packed = ((static_cast<uint32_t>(rgb48[0]) >> 6) << 20) |
                                  ((static_cast<uint32_t>(rgb48[1]) >> 6) << 10) | (static_cast<uint32_t>(rgb48[2]) >> 6);
          std::memcpy(pixels.data() + y * pitch + x * 4, &packed, sizeof(packed));
        } else {
          const int offset = y * pitch + 3 * x;
          pixels[offset] = static_cast<uint8_t>(r);
          pixels[offset + 1] = static_cast<uint8_t>(g);
          pixels[offset + 2] = static_cast<uint8_t>(b);
        }
      }
    }
    return pixels;
  }
};

bool upload(SDL_Texture* texture, const std::vector<uint8_t>& pixels, int start, int end, bool ten_bpc) {
  if (start >= end) return true;
  const SDL_Rect region{start, 0, end - start, kHeight};
  return SDL_UpdateTexture(texture, &region, pixels.data() + (ten_bpc ? 4 : 3) * start, ten_bpc ? kPitch10 : kPitch) == 0;
}

bool render(SDL_Renderer* renderer, SDL_Texture* texture, int split, bool show_left, bool show_right, std::vector<uint32_t>& pixels) {
  SDL_SetRenderDrawColor(renderer, 13, 28, 43, 255);
  if (SDL_RenderClear(renderer) != 0) return false;
  const int left_end = show_left ? (show_right ? split : kWidth) : 0;
  const int right_start = show_right ? (show_left ? split : 0) : kWidth;
  if (left_end > 0) {
    SDL_Rect roi{0, 0, left_end, kHeight};
    if (SDL_RenderCopy(renderer, texture, &roi, &roi) != 0) return false;
  }
  if (right_start < kWidth) {
    SDL_Rect roi{right_start, 0, kWidth - right_start, kHeight};
    if (SDL_RenderCopy(renderer, texture, &roi, &roi) != 0) return false;
  }
  return SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, pixels.data(), kWidth * sizeof(uint32_t)) == 0;
}

}  // namespace

int main(int argc, char** argv) {
  bool accelerated = false;
  bool ten_bpc = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--accelerated") == 0) {
      accelerated = true;
    } else if (std::strcmp(argv[i], "--10-bpc") == 0) {
      ten_bpc = true;
    } else {
      std::fprintf(stderr, "Usage: test_split_texture_upload_cache [--accelerated] [--10-bpc]\n");
      return EXIT_FAILURE;
    }
  }
  if (!accelerated) SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }

  SDL_Surface* surface = nullptr;
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  if (accelerated) {
    window = SDL_CreateWindow("GPU upload equivalence", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, kWidth, kHeight, SDL_WINDOW_HIDDEN);
    if (window) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  } else {
    surface = SDL_CreateRGBSurfaceWithFormat(0, kWidth, kHeight, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface) renderer = SDL_CreateSoftwareRenderer(surface);
  }
  if (!renderer) {
    std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }
  SDL_RendererInfo info{};
  SDL_GetRendererInfo(renderer, &info);
  if (accelerated && !(info.flags & SDL_RENDERER_ACCELERATED)) {
    std::fprintf(stderr, "Requested accelerated renderer but got %s\n", info.name);
    return EXIT_FAILURE;
  }

  auto make_texture = [&]() { return SDL_CreateTexture(renderer, ten_bpc ? SDL_PIXELFORMAT_ARGB2101010 : SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, kWidth, kHeight); };
  SDL_Texture* reference = make_texture();
  SDL_Texture* optimized = make_texture();
  if (!reference || !optimized) {
    std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }

  SplitTextureUploadCache cache;
  cache.reset(kWidth);
  // Equal timestamps alone must not suppress uploads on swaps or when a
  // different right-hand input is selected.
  int source_a = 0, source_b = 0, source_c = 0;
  const int middle = kWidth / 2;
  cache.set_frames(&source_a, "0:0", &source_b, "0:0");
  cache.left_uploaded(middle);
  cache.right_uploaded(middle);
  if (cache.needs_left_upload(middle) || cache.needs_right_upload(middle)) {
    std::fprintf(stderr, "FAIL unchanged frame identity invalidated cached pixels\n");
    return EXIT_FAILURE;
  }
  cache.set_frames(&source_b, "0:0", &source_a, "0:0");
  if (!cache.needs_left_upload(middle) || !cache.needs_right_upload(middle)) {
    std::fprintf(stderr, "FAIL swapped identical-key frames kept stale pixels\n");
    return EXIT_FAILURE;
  }
  cache.left_uploaded(middle);
  cache.right_uploaded(middle);
  cache.set_frames(&source_b, "0:0", &source_c, "0:0");
  if (cache.needs_left_upload(middle) || !cache.needs_right_upload(middle)) {
    std::fprintf(stderr, "FAIL active-right input change failed to invalidate right pixels\n");
    return EXIT_FAILURE;
  }
  cache.reset(kWidth);
  bool cache_active = false;
  // Source frames deliberately share timestamp keys across distinct inputs.
  std::array<Frame, 4> frames{{{11}, {22}, {33}, {44}}};
  std::mt19937 random(20261125);
  std::vector<uint32_t> expected(kWidth * kHeight), actual(kWidth * kHeight);
  int comparisons = 0;
  int cache_updates = 0;
  int reference_updates = 0;
  int stationary_cache_updates = 0;
  int stationary_reference_updates = 0;

  // First verify a long sweep with unchanged paused frames pixel-for-pixel,
  // then exercise swaps, changing frames, source changes and texture resets.
  constexpr int stationary_cases = 400;
  constexpr int randomized_cases = 2000;
  for (int step = 0; step < stationary_cases + randomized_cases; ++step) {
    const bool stationary = step < stationary_cases;
    const int left_index = stationary ? 0 : static_cast<int>(random() % frames.size());
    const int right_index = stationary ? 1 : static_cast<int>(random() % frames.size());
    if (!stationary && random() % 7 == 0) ++frames[left_index].generation;
    if (!stationary && random() % 7 == 0) ++frames[right_index].generation;
    const Frame& left = frames[left_index];
    const Frame& right = frames[right_index];

    const int split = stationary ? (step == 0 ? 0 : (step == 1 ? kWidth : 15 + (step * 37) % (kWidth - 30)))
                                 : static_cast<int>(random() % (kWidth + 1));
    const bool show_left = stationary || random() % 13 != 0;
    const bool show_right = stationary || random() % 13 != 0;
    // Disabled cases represent playback, subtraction and stacked modes.
    const bool enabled = stationary || random() % 9 != 0;
    const bool recreate = !stationary && (step - stationary_cases) % 101 == 0;
    if (recreate) {
      SDL_DestroyTexture(reference);
      SDL_DestroyTexture(optimized);
      reference = make_texture();
      optimized = make_texture();
      if (!reference || !optimized) {
        std::fprintf(stderr, "SDL texture recreation failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
      }
      cache_active = false;
    }
    if (!enabled) {
      if (cache_active) {
        cache.reset(kWidth);
        cache_active = false;
      }
    } else {
      if (!cache_active) {
        cache.reset(kWidth);
        cache_active = true;
      }
      cache.set_frames(&left, left.key(), &right, right.key());
    }

    const auto left_pixels = left.pixels(ten_bpc);
    const auto right_pixels = right.pixels(ten_bpc);
    const int left_end = show_left ? (show_right ? split : kWidth) : 0;
    const int right_start = show_right ? (show_left ? split : 0) : kWidth;

    // Baseline: always refresh the current visible regions.
    if (left_end > 0) {
      if (!upload(reference, left_pixels, 0, left_end, ten_bpc)) {
        std::fprintf(stderr, "Reference left update failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
      }
      ++reference_updates;
      if (stationary) ++stationary_reference_updates;
    }
    if (right_start < kWidth) {
      if (!upload(reference, right_pixels, right_start, kWidth, ten_bpc)) {
        std::fprintf(stderr, "Reference right update failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
      }
      ++reference_updates;
      if (stationary) ++stationary_reference_updates;
    }

    // Optimize the paused split view in both 8-bit and 10-bit modes.
    if (left_end > 0 && (!enabled || cache.needs_left_upload(left_end))) {
      if (!upload(optimized, left_pixels, 0, left_end, ten_bpc)) {
        std::fprintf(stderr, "Optimized left update failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
      }
      if (enabled) cache.left_uploaded(left_end);
      ++cache_updates;
      if (stationary) ++stationary_cache_updates;
    }
    if (right_start < kWidth && (!enabled || cache.needs_right_upload(right_start))) {
      if (!upload(optimized, right_pixels, right_start, kWidth, ten_bpc)) {
        std::fprintf(stderr, "Optimized right update failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
      }
      if (enabled) cache.right_uploaded(right_start);
      ++cache_updates;
      if (stationary) ++stationary_cache_updates;
    }

    if (!render(renderer, reference, split, show_left, show_right, expected) ||
        !render(renderer, optimized, split, show_left, show_right, actual)) {
      std::fprintf(stderr, "SDL pixel readback failed: %s\n", SDL_GetError());
      return EXIT_FAILURE;
    }
    // A reference and optimized path both rendering black would also
    // compare equal. Ensure the packed 10-bit texture really renders.
    if (ten_bpc && step == 1 &&
        std::all_of(expected.begin(), expected.end(), [&](uint32_t pixel) { return pixel == expected.front(); })) {
      std::fprintf(stderr, "FAIL 10-bpc texture did not render image detail\n");
      return EXIT_FAILURE;
    }
    if (actual != expected) {
      auto pos = std::mismatch(actual.begin(), actual.end(), expected.begin());
      const int index = static_cast<int>(pos.first - actual.begin());
      std::fprintf(stderr, "FAIL pixels: step=%d split=%d left=%d right=%d shown=%d/%d enabled=%d pixel=(%d,%d) got=%08x expected=%08x\n",
                   step, split, left_index, right_index, int(show_left), int(show_right), int(enabled),
                   index % kWidth, index / kWidth, *pos.first, *pos.second);
      return EXIT_FAILURE;
    }
    ++comparisons;
  }

  // The stationary sweep verified the pixels, not just the upload counts.
  if (stationary_cache_updates >= stationary_reference_updates) {
    std::fprintf(stderr, "FAIL no upload reduction over the stationary pixel-equivalence sweep\n");
    return EXIT_FAILURE;
  }

  SDL_DestroyTexture(reference);
  SDL_DestroyTexture(optimized);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_FreeSurface(surface);
  SDL_Quit();
  std::printf("PASS %d split texture pixel-equivalence cases (%s, %s), cache uploads %d/%d; stationary %d/%d\n",
              comparisons, info.name, ten_bpc ? "10 bpc" : "8 bpc", cache_updates, reference_updates, stationary_cache_updates, stationary_reference_updates);
  return EXIT_SUCCESS;
}
