#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
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
          const uint32_t packed = (3u << 30) | (static_cast<uint32_t>(r) << 20) | (static_cast<uint32_t>(g) << 10) | static_cast<uint32_t>(b);
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
  if (start >= end)
    return true;
  const SDL_Rect region{start, 0, end - start, kHeight};
  return SDL_UpdateTexture(texture, &region, pixels.data() + (ten_bpc ? 4 : 3) * start, ten_bpc ? kPitch10 : kPitch);
}

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

bool render(SDL_Renderer* renderer, SDL_Texture* texture, int split, bool show_left, bool show_right, std::vector<uint32_t>& pixels) {
  SDL_SetRenderDrawColor(renderer, 13, 28, 43, 255);
  if (!SDL_RenderClear(renderer))
    return false;
  const int left_end = show_left ? (show_right ? split : kWidth) : 0;
  const int right_start = show_right ? (show_left ? split : 0) : kWidth;
  if (left_end > 0) {
    const SDL_FRect roi{0.f, 0.f, static_cast<float>(left_end), static_cast<float>(kHeight)};
    if (!SDL_RenderTexture(renderer, texture, &roi, &roi))
      return false;
  }
  if (right_start < kWidth) {
    const SDL_FRect roi{static_cast<float>(right_start), 0.f, static_cast<float>(kWidth - right_start), static_cast<float>(kHeight)};
    if (!SDL_RenderTexture(renderer, texture, &roi, &roi))
      return false;
  }
  return read_renderer_argb(renderer, kWidth, kHeight, pixels);
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
  if (!accelerated) {
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "software", SDL_HINT_OVERRIDE);
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }

  SDL_Surface* surface = nullptr;
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  std::string renderer_name = "software";
  if (accelerated) {
    window = SDL_CreateWindow("GPU upload equivalence", kWidth, kHeight, SDL_WINDOW_HIDDEN);
    if (window) {
      const SDL_PropertiesID props = SDL_CreateProperties();
      SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
      SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER, SDL_COLORSPACE_SRGB);
      renderer = SDL_CreateRendererWithProperties(props);
      SDL_DestroyProperties(props);
    }
  } else {
    surface = SDL_CreateSurface(kWidth, kHeight, SDL_PIXELFORMAT_ARGB8888);
    if (surface)
      renderer = SDL_CreateSoftwareRenderer(surface);
  }
  if (!renderer) {
    std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }
  if (const char* name = SDL_GetRendererName(renderer)) {
    renderer_name = name;
  }
  if (accelerated && renderer_name.find("software") != std::string::npos) {
    std::fprintf(stderr, "Requested accelerated renderer but got %s\n", renderer_name.c_str());
    return EXIT_FAILURE;
  }

  auto make_texture = [&]() {
    const SDL_PropertiesID props = SDL_CreateProperties();
    const SDL_PixelFormat format = ten_bpc ? SDL_PIXELFORMAT_ARGB2101010 : SDL_PIXELFORMAT_RGB24;
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, format);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, kWidth);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, kHeight);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, SDL_COLORSPACE_SRGB);
    SDL_Texture* texture = SDL_CreateTextureWithProperties(renderer, props);
    SDL_DestroyProperties(props);
    return texture;
  };
  SDL_Texture* reference = make_texture();
  SDL_Texture* optimized = make_texture();
  if (!reference || !optimized) {
    std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }

  // 10-bit shading/readback needs a GPU-backed renderer; dummy software still
  // validates upload-only paths when --10-bpc is used without --accelerated.
  const bool trust_render_pixels = !ten_bpc || accelerated;

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
    if (!stationary && random() % 7 == 0)
      ++frames[left_index].generation;
    if (!stationary && random() % 7 == 0)
      ++frames[right_index].generation;
    const Frame& left = frames[left_index];
    const Frame& right = frames[right_index];

    const int split = stationary ? (step == 0 ? 0 : (step == 1 ? kWidth : 15 + (step * 37) % (kWidth - 30))) : static_cast<int>(random() % (kWidth + 1));
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
      if (stationary)
        ++stationary_reference_updates;
    }
    if (right_start < kWidth) {
      if (!upload(reference, right_pixels, right_start, kWidth, ten_bpc)) {
        std::fprintf(stderr, "Reference right update failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
      }
      ++reference_updates;
      if (stationary)
        ++stationary_reference_updates;
    }

    // Optimize the paused split view in both 8-bit and 10-bit modes.
    if (left_end > 0 && (!enabled || cache.needs_left_upload(left_end))) {
      if (!upload(optimized, left_pixels, 0, left_end, ten_bpc)) {
        std::fprintf(stderr, "Optimized left update failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
      }
      if (enabled)
        cache.left_uploaded(left_end);
      ++cache_updates;
      if (stationary)
        ++stationary_cache_updates;
    }
    if (right_start < kWidth && (!enabled || cache.needs_right_upload(right_start))) {
      if (!upload(optimized, right_pixels, right_start, kWidth, ten_bpc)) {
        std::fprintf(stderr, "Optimized right update failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
      }
      if (enabled)
        cache.right_uploaded(right_start);
      ++cache_updates;
      if (stationary)
        ++stationary_cache_updates;
    }

    if (trust_render_pixels) {
      if (!render(renderer, reference, split, show_left, show_right, expected) || !render(renderer, optimized, split, show_left, show_right, actual)) {
        std::fprintf(stderr, "SDL pixel readback failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
      }
      // A reference and optimized path both rendering black would also
      // compare equal. Ensure the packed 10-bit texture really renders.
      if (ten_bpc && step == 1 && left_end == kWidth) {
        const auto unique_end = std::unique(expected.begin(), expected.end());
        if (unique_end == expected.begin() + 1) {
          std::fprintf(stderr, "FAIL 10-bpc texture did not render image detail (flat #%08x)\n", expected.front());
          return EXIT_FAILURE;
        }
      }
      if (actual != expected) {
        auto pos = std::mismatch(actual.begin(), actual.end(), expected.begin());
        const int index = static_cast<int>(pos.first - actual.begin());
        std::fprintf(stderr, "FAIL pixels: step=%d split=%d left=%d right=%d shown=%d/%d enabled=%d pixel=(%d,%d) got=%08x expected=%08x\n", step, split, left_index, right_index, int(show_left), int(show_right), int(enabled), index % kWidth,
                     index / kWidth, *pos.first, *pos.second);
        return EXIT_FAILURE;
      }
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
  SDL_DestroySurface(surface);
  SDL_Quit();
  std::printf("PASS %d split texture %s cases (%s, %s), cache uploads %d/%d; stationary %d/%d\n", comparisons, trust_render_pixels ? "pixel-equivalence" : "upload-only", renderer_name.c_str(), ten_bpc ? "10 bpc" : "8 bpc", cache_updates, reference_updates, stationary_cache_updates,
              stationary_reference_updates);
  return EXIT_SUCCESS;
}
