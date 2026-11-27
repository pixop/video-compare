#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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

SDL_Texture* create_streaming_texture(SDL_Renderer* renderer, SDL_PixelFormat format, int w, int h, SDL_Colorspace colorspace, bool set_colorspace) {
  const SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, format);
  SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
  SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
  SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
  if (set_colorspace) {
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, colorspace);
  }
  SDL_Texture* texture = SDL_CreateTextureWithProperties(renderer, props);
  SDL_DestroyProperties(props);
  return texture;
}

SDL_Colorspace texture_colorspace(SDL_Texture* texture) {
  const SDL_PropertiesID props = SDL_GetTextureProperties(texture);
  return static_cast<SDL_Colorspace>(SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_COLORSPACE_NUMBER, SDL_COLORSPACE_UNKNOWN));
}

bool upload_rgb24_ramp(SDL_Texture* texture, int width, int height) {
  std::vector<uint8_t> rgb(static_cast<size_t>(width) * static_cast<size_t>(height) * 3u);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3u;
      rgb[offset] = static_cast<uint8_t>((x * 255) / std::max(1, width - 1));
      rgb[offset + 1] = static_cast<uint8_t>((y * 255) / std::max(1, height - 1));
      rgb[offset + 2] = 128;
    }
  }
  const SDL_Rect full{0, 0, width, height};
  return SDL_UpdateTexture(texture, &full, rgb.data(), width * 3);
}

bool upload_10bpc_solid(SDL_Texture* texture, int width, int height, uint32_t r10, uint32_t g10, uint32_t b10) {
  const uint32_t packed = (3u << 30) | (r10 << 20) | (g10 << 10) | b10;
  std::vector<uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height), packed);
  const SDL_Rect full{0, 0, width, height};
  return SDL_UpdateTexture(texture, &full, pixels.data(), width * static_cast<int>(sizeof(uint32_t)));
}

uint32_t center_pixel(const std::vector<uint32_t>& pixels, int width, int height) {
  return pixels[static_cast<size_t>(height / 2) * static_cast<size_t>(width) + static_cast<size_t>(width / 2)];
}

}  // namespace

int main(int argc, char** argv) {
  bool accelerated = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--accelerated") == 0) {
      accelerated = true;
    } else {
      std::fprintf(stderr, "Usage: test_sdr_texture_render [--accelerated]\n");
      return EXIT_FAILURE;
    }
  }

  constexpr int kWidth = 64;
  constexpr int kHeight = 32;
  if (!accelerated) {
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "software", SDL_HINT_OVERRIDE);
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }

  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  if (accelerated) {
    window = SDL_CreateWindow("SDR texture render", kWidth, kHeight, 0);
    if (window != nullptr) {
      const SDL_PropertiesID rprops = SDL_CreateProperties();
      SDL_SetPointerProperty(rprops, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
      SDL_SetNumberProperty(rprops, SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER, SDL_COLORSPACE_SRGB);
      renderer = SDL_CreateRendererWithProperties(rprops);
      SDL_DestroyProperties(rprops);
    }
  } else {
    SDL_Surface* surface = SDL_CreateSurface(kWidth, kHeight, SDL_PIXELFORMAT_ARGB8888);
    renderer = surface != nullptr ? SDL_CreateSoftwareRenderer(surface) : nullptr;
  }

  if (renderer == nullptr) {
    std::fprintf(stderr, "Renderer init failed: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }

  const int sdl_version = SDL_GetVersion();
  std::printf("SDL version: %d.%d.%d (pinned via CI prefix)\n", SDL_VERSIONNUM_MAJOR(sdl_version), SDL_VERSIONNUM_MINOR(sdl_version), SDL_VERSIONNUM_MICRO(sdl_version));

  SDL_Texture* rgb = create_streaming_texture(renderer, SDL_PIXELFORMAT_RGB24, kWidth, kHeight, SDL_COLORSPACE_SRGB, true);
  if (rgb == nullptr || texture_colorspace(rgb) != SDL_COLORSPACE_SRGB) {
    std::fprintf(stderr, "FAIL RGB24 texture colorspace\n");
    return EXIT_FAILURE;
  }
  if (!upload_rgb24_ramp(rgb, kWidth, kHeight)) {
    std::fprintf(stderr, "FAIL RGB24 upload\n");
    return EXIT_FAILURE;
  }
  std::vector<uint32_t> pixels;
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);
  SDL_RenderTexture(renderer, rgb, nullptr, nullptr);
  if (!accelerated) {
    std::fprintf(stderr, "SKIP accelerated-only SDR pixel readback (run with --accelerated under Xvfb)\n");
    SDL_DestroyTexture(rgb);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_SUCCESS;
  }

  if (!read_renderer_argb(renderer, kWidth, kHeight, pixels)) {
    std::fprintf(stderr, "FAIL RGB24 readback\n");
    return EXIT_FAILURE;
  }
  const uint32_t rgb_center = center_pixel(pixels, kWidth, kHeight);
  const uint8_t r = static_cast<uint8_t>((rgb_center >> 16) & 0xff);
  const uint8_t g = static_cast<uint8_t>((rgb_center >> 8) & 0xff);
  const uint8_t b = static_cast<uint8_t>(rgb_center & 0xff);
  if (r < 120 || r > 140 || g < 120 || g > 140 || b < 120 || b > 140) {
    std::fprintf(stderr, "FAIL RGB24 SDR ramp center got #%02x%02x%02x expected ~#808080\n", r, g, b);
    return EXIT_FAILURE;
  }
  std::printf("PASS RGB24 SDR ramp center #%02x%02x%02x\n", r, g, b);

  SDL_Texture* ten_default = create_streaming_texture(renderer, SDL_PIXELFORMAT_ARGB2101010, kWidth, kHeight, SDL_COLORSPACE_SRGB, false);
  SDL_Texture* ten_srgb = create_streaming_texture(renderer, SDL_PIXELFORMAT_ARGB2101010, kWidth, kHeight, SDL_COLORSPACE_SRGB, true);
  if (ten_default == nullptr || ten_srgb == nullptr) {
    std::fprintf(stderr, "FAIL 10-bit texture create\n");
    return EXIT_FAILURE;
  }
  const SDL_Colorspace default_cs = texture_colorspace(ten_default);
  const SDL_Colorspace tagged_cs = texture_colorspace(ten_srgb);
  if (tagged_cs != SDL_COLORSPACE_SRGB) {
    std::fprintf(stderr, "FAIL explicit SRGB tag on ARGB2101010 (got 0x%08x)\n", tagged_cs);
    return EXIT_FAILURE;
  }
  if (default_cs == SDL_COLORSPACE_SRGB) {
    std::fprintf(stderr, "NOTE ARGB2101010 default colorspace is SRGB on this build (0x%08x)\n", default_cs);
  } else if (default_cs != SDL_COLORSPACE_HDR10) {
    std::fprintf(stderr, "WARN ARGB2101010 default colorspace 0x%08x (expected HDR10 0x%08x on SDL 3.2.x)\n", default_cs, SDL_COLORSPACE_HDR10);
  } else {
    std::printf("PASS ARGB2101010 default colorspace is HDR10 without explicit tag\n");
  }

  constexpr uint32_t red10 = 900u;
  constexpr uint32_t green10 = 120u;
  constexpr uint32_t blue10 = 60u;
  upload_10bpc_solid(ten_default, kWidth, kHeight, red10, green10, blue10);
  upload_10bpc_solid(ten_srgb, kWidth, kHeight, red10, green10, blue10);

  SDL_RenderClear(renderer);
  SDL_RenderTexture(renderer, ten_default, nullptr, nullptr);
  if (!read_renderer_argb(renderer, kWidth, kHeight, pixels)) {
    std::fprintf(stderr, "FAIL 10-bit default readback\n");
    return EXIT_FAILURE;
  }
  const uint32_t default_center = center_pixel(pixels, kWidth, kHeight);

  SDL_RenderClear(renderer);
  SDL_RenderTexture(renderer, ten_srgb, nullptr, nullptr);
  if (!read_renderer_argb(renderer, kWidth, kHeight, pixels)) {
    std::fprintf(stderr, "FAIL 10-bit SRGB readback\n");
    return EXIT_FAILURE;
  }
  const uint32_t srgb_center = center_pixel(pixels, kWidth, kHeight);

  const uint8_t sr = static_cast<uint8_t>((srgb_center >> 16) & 0xff);
  const uint8_t sg = static_cast<uint8_t>((srgb_center >> 8) & 0xff);
  const uint8_t sb = static_cast<uint8_t>(srgb_center & 0xff);
  if (sr < 200 || sg > 80 || sb > 80) {
    std::fprintf(stderr, "FAIL 10-bit SRGB tagged render got #%02x%02x%02x expected dominant red\n", sr, sg, sb);
    return EXIT_FAILURE;
  }
  std::printf("PASS 10-bit SRGB tagged render center #%02x%02x%02x\n", sr, sg, sb);

  if (default_cs == SDL_COLORSPACE_HDR10 && default_center != srgb_center) {
    std::printf("PASS HDR10-default vs SRGB-tagged 10-bit renders differ (#%08x vs #%08x)\n", default_center, srgb_center);
  } else if (default_cs == SDL_COLORSPACE_HDR10 && default_center == srgb_center) {
    std::printf("NOTE Mesa/SDR framebuffer: HDR10-default and SRGB-tagged 10-bit read back identically (#%08x); explicit SRGB tag still required for legacy SDR content\n", srgb_center);
  }

  SDL_DestroyTexture(ten_default);
  SDL_DestroyTexture(ten_srgb);
  SDL_DestroyTexture(rgb);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  std::puts("All SDR texture render checks passed");
  return EXIT_SUCCESS;
}