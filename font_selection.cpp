#include "font_selection.h"
#include <cstdint>
#include <stdexcept>

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

Display::EmbeddedFont embedded_font_for_forced_mode(FontMode mode) {
  switch (mode) {
    case FontMode::SourceCodePro:
      return Display::EmbeddedFont::SourceCodePro;
    case FontMode::Sarasa:
      return Display::EmbeddedFont::Sarasa;
    case FontMode::Auto:
      throw std::logic_error{"embedded_font_for_forced_mode called with FontMode::Auto"};
  }
  throw std::logic_error{"unknown FontMode"};
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

Display::EmbeddedFont resolve_auto_embedded_font(TTF_Font* scp_probe_font,
                                        const std::string& left_label,
                                        const std::string& right_label,
                                        bool* malformed_utf8_out) {
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

  return left_supported && right_supported ? Display::EmbeddedFont::SourceCodePro : Display::EmbeddedFont::Sarasa;
}
