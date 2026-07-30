#pragma once
#include <SDL2/SDL_ttf.h>
#include <string>
#include "core_types.h"
#include "display.h"

// Forced modes only — throws std::logic_error if mode == Auto.
[[nodiscard]] Display::EmbeddedFont embedded_font_for_forced_mode(FontMode mode);

// Returns false if UTF-8 malformed or any required glyph missing.
[[nodiscard]] bool font_supports_utf8_text(TTF_Font* font, const std::string& text, bool* malformed_utf8_out = nullptr);

[[nodiscard]] Display::EmbeddedFont resolve_auto_embedded_font(TTF_Font* scp_probe_font,
                                                               const std::string& left_label,
                                                               const std::string& right_label,
                                                               bool* malformed_utf8_out = nullptr);
