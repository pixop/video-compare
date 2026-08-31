#include "font_selection.h"
#include <cstdint>
#include <stdexcept>
#include "sarasa_mono_sc_regular_ttf.h"
#include "source_code_pro_regular_ttf.h"
#include "string_utils.h"
extern "C" {
#include <SDL2/SDL.h>
}

#if !SDL_TTF_VERSION_ATLEAST(2, 0, 18)
#error "SDL2_ttf >= 2.0.18 required for TTF_GlyphIsProvided32"
#endif

namespace {

bool codepoint_requires_glyph(uint32_t cp) {
  return cp > 0x001F && cp != 0x007F;
}

bool decode_next_utf8_codepoint(const char*& ptr, const char* end, uint32_t& cp_out) {
  if (ptr >= end) {
    return false;
  }

  const unsigned char b0 = static_cast<unsigned char>(*ptr);

  if (b0 <= 0x7F) {
    cp_out = b0;
    ++ptr;
    return true;
  }

  if (b0 >= 0xC2 && b0 <= 0xDF) {
    if (end - ptr < 2) {
      return false;
    }
    const unsigned char b1 = static_cast<unsigned char>(ptr[1]);
    if ((b1 & 0xC0) != 0x80) {
      return false;
    }
    cp_out = ((static_cast<uint32_t>(b0) & 0x1F) << 6) | (static_cast<uint32_t>(b1) & 0x3F);
    if (cp_out < 0x80) {
      return false;
    }
    ptr += 2;
    return true;
  }

  if (b0 >= 0xE0 && b0 <= 0xEF) {
    if (end - ptr < 3) {
      return false;
    }
    const unsigned char b1 = static_cast<unsigned char>(ptr[1]);
    const unsigned char b2 = static_cast<unsigned char>(ptr[2]);
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
      return false;
    }
    cp_out = ((static_cast<uint32_t>(b0) & 0x0F) << 12) | ((static_cast<uint32_t>(b1) & 0x3F) << 6) | (static_cast<uint32_t>(b2) & 0x3F);
    if (b0 == 0xE0 && b1 < 0xA0) {
      return false;
    }
    if (b0 == 0xED && b1 >= 0xA0) {
      return false;
    }
    if (cp_out < 0x800) {
      return false;
    }
    ptr += 3;
    return true;
  }

  if (b0 >= 0xF0 && b0 <= 0xF4) {
    if (end - ptr < 4) {
      return false;
    }
    const unsigned char b1 = static_cast<unsigned char>(ptr[1]);
    const unsigned char b2 = static_cast<unsigned char>(ptr[2]);
    const unsigned char b3 = static_cast<unsigned char>(ptr[3]);
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
      return false;
    }
    cp_out = ((static_cast<uint32_t>(b0) & 0x07) << 18) | ((static_cast<uint32_t>(b1) & 0x3F) << 12) | ((static_cast<uint32_t>(b2) & 0x3F) << 6) | (static_cast<uint32_t>(b3) & 0x3F);
    if (b0 == 0xF0 && b1 < 0x90) {
      return false;
    }
    if (b0 == 0xF4 && b1 > 0x8F) {
      return false;
    }
    if (cp_out < 0x10000 || cp_out > 0x10FFFF) {
      return false;
    }
    ptr += 4;
    return true;
  }

  return false;
}

}  // namespace

namespace {

template <typename T>
T check_sdl(T value, const char* context) {
  if (!value) {
    throw std::runtime_error{std::string("SDL ") + context + " - " + SDL_GetError()};
  }
  return value;
}

[[nodiscard]] TTF_Font* open_embedded_font(EmbeddedFont family, int point_size, const char* text_role) {
  const char* family_name = embedded_font_display_name(family);
  SDL_RWops* rw = nullptr;

  switch (family) {
    case EmbeddedFont::SourceCodePro:
      rw = check_sdl(SDL_RWFromConstMem(SOURCE_CODE_PRO_REGULAR_TTF, SOURCE_CODE_PRO_REGULAR_TTF_LEN), "get pointer to font");
      break;
    case EmbeddedFont::Sarasa:
      rw = check_sdl(SDL_RWFromConstMem(SARASA_MONO_SC_REGULAR_TTF, SARASA_MONO_SC_REGULAR_TTF_LEN), "get pointer to font");
      break;
  }

  TTF_Font* font = TTF_OpenFontRW(rw, 1, point_size);
  if (font == nullptr) {
    const char* ttf_error = TTF_GetError();
    const std::string raw_error = (ttf_error != nullptr && *ttf_error != '\0') ? ttf_error : "unknown error";
    throw std::runtime_error{string_sprintf("TTF open %s %s (%d pt) - %s", family_name, text_role, point_size, raw_error.c_str())};
  }
  return font;
}

}  // namespace

EmbeddedFont embedded_font_for_forced_mode(FontMode mode) {
  switch (mode) {
    case FontMode::SourceCodePro:
      return EmbeddedFont::SourceCodePro;
    case FontMode::Sarasa:
      return EmbeddedFont::Sarasa;
    case FontMode::Auto:
      throw std::logic_error{"embedded_font_for_forced_mode called with FontMode::Auto"};
    case FontMode::CustomFile:
      throw std::logic_error{"embedded_font_for_forced_mode called with FontMode::CustomFile"};
  }
  throw std::logic_error{"unknown FontMode"};
}

const char* embedded_font_display_name(EmbeddedFont font) {
  switch (font) {
    case EmbeddedFont::SourceCodePro:
      return "Source Code Pro";
    case EmbeddedFont::Sarasa:
      return "Sarasa Mono SC";
  }
  return "unknown font";
}

TTF_Font* open_ui_font_at_size(const FontSelection& selection, EmbeddedFont active_embedded_font, int point_size, const char* text_role) {
  if (selection.mode == FontMode::CustomFile) {
    if (selection.custom_file_path.empty()) {
      throw std::logic_error{"CustomFile font mode requires a non-empty path"};
    }
    return open_custom_font_file(selection.custom_file_path, point_size, text_role);
  }
  return open_embedded_font(active_embedded_font, point_size, text_role);
}

namespace {

// SDL_RWFromFile names the path in every current build:
//   Windows / older Unix: "Couldn't open <path>"
//   newer Unix/macOS:     "Couldn't open <path>: <strerror>"
std::string strip_duplicate_path_from_ttf_error(const std::string& path, const std::string& ttf_error) {
  const std::string prefix = "Couldn't open " + path;
  if (ttf_error.compare(0, prefix.size(), prefix) != 0) {
    return ttf_error;
  }

  size_t i = prefix.size();
  while (i < ttf_error.size() && (ttf_error[i] == '\r' || ttf_error[i] == '\n')) {
    ++i;
  }
  if (i == ttf_error.size()) {
    return {};
  }
  if (ttf_error[i] == ':') {
    ++i;
    if (i < ttf_error.size() && ttf_error[i] == ' ') {
      ++i;
    }
    return ttf_error.substr(i);
  }
  return ttf_error;
}

}  // namespace

std::string format_custom_font_open_error(const std::string& path, int point_size, const char* text_role, const char* ttf_error) {
  const std::string raw_error = (ttf_error != nullptr && *ttf_error != '\0') ? ttf_error : "unknown error";
  std::string reason = strip_duplicate_path_from_ttf_error(path, raw_error);
  if (reason.empty()) {
    reason = "unknown error";
  }
  return string_sprintf("failed to open custom UI font '%s' for %s (%d pt): %s", path.c_str(), text_role, point_size, reason.c_str());
}

TTF_Font* open_custom_font_file(const std::string& path, int point_size, const char* text_role) {
  TTF_Font* font = TTF_OpenFont(path.c_str(), point_size);
  if (font == nullptr) {
    const char* ttf_error = TTF_GetError();
    throw std::runtime_error{format_custom_font_open_error(path, point_size, text_role, ttf_error)};
  }
  return font;
}

bool font_supports_utf8_text(TTF_Font* font, const std::string& text, bool* malformed_utf8_out) {
  if (font == nullptr) {
    throw std::logic_error{"font_supports_utf8_text called with null TTF_Font"};
  }

  if (malformed_utf8_out != nullptr) {
    *malformed_utf8_out = false;
  }

  const char* ptr = text.data();
  const char* end = ptr + text.size();
  uint32_t cp = 0;
  bool all_glyphs_supported = true;

  while (ptr < end) {
    if (!decode_next_utf8_codepoint(ptr, end, cp)) {
      if (malformed_utf8_out != nullptr) {
        *malformed_utf8_out = true;
      }
      return false;
    }

    if (all_glyphs_supported && codepoint_requires_glyph(cp) && !TTF_GlyphIsProvided32(font, static_cast<Uint32>(cp))) {
      all_glyphs_supported = false;
    }
  }

  return all_glyphs_supported;
}

EmbeddedFont resolve_auto_embedded_font(TTF_Font* scp_probe_font, const std::string& left_label, const std::string& right_label, bool* malformed_utf8_out) {
  if (malformed_utf8_out != nullptr) {
    *malformed_utf8_out = false;
  }

  bool left_malformed = false;
  bool right_malformed = false;

  const bool left_supported = font_supports_utf8_text(scp_probe_font, left_label, &left_malformed);
  const bool right_supported = font_supports_utf8_text(scp_probe_font, right_label, &right_malformed);

  if (malformed_utf8_out != nullptr) {
    *malformed_utf8_out = left_malformed || right_malformed;
  }

  return left_supported && right_supported ? EmbeddedFont::SourceCodePro : EmbeddedFont::Sarasa;
}
