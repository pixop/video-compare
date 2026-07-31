#pragma once
#include <SDL2/SDL_ttf.h>
#include <string>
#include "core_types.h"

enum class EmbeddedFont { SourceCodePro, Sarasa };

// Forced modes only — throws std::logic_error if mode == Auto or CustomFile.
[[nodiscard]] EmbeddedFont embedded_font_for_forced_mode(FontMode mode);

[[nodiscard]] const char* embedded_font_display_name(EmbeddedFont font);

[[nodiscard]] TTF_Font* open_custom_font_file(const std::string& path, int point_size, const char* text_role);
// text_role must be a controlled literal such as "small text" or "large text".

[[nodiscard]] std::string format_custom_font_open_error(const std::string& path,
                                                        int point_size,
                                                        const char* text_role,
                                                        const char* ttf_error);

[[nodiscard]] TTF_Font* open_ui_font_at_size(const FontSelection& selection,
                                             EmbeddedFont active_embedded_font,
                                             int point_size,
                                             const char* text_role);

// Returns false if UTF-8 malformed or any required glyph missing.
[[nodiscard]] bool font_supports_utf8_text(TTF_Font* font, const std::string& text, bool* malformed_utf8_out = nullptr);

[[nodiscard]] EmbeddedFont resolve_auto_embedded_font(TTF_Font* scp_probe_font,
                                                      const std::string& left_label,
                                                      const std::string& right_label,
                                                      bool* malformed_utf8_out = nullptr);
