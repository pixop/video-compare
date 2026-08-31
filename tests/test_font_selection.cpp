#define SDL_MAIN_HANDLED
#include "font_selection.h"
#include "source_code_pro_regular_ttf.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
}

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#ifndef O_BINARY
#define O_BINARY 0
#endif
#else
#include <unistd.h>
#endif

static int failures = 0;

static void expect(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s\n", message);
    failures++;
    return;
  }
  std::printf("PASS %s\n", message);
}

struct SdlTtfFixture {
  SdlTtfFixture() {
    if (SDL_Init(0) != 0) {
      throw std::runtime_error{std::string("SDL_Init failed: ") + SDL_GetError()};
    }
    if (TTF_Init() != 0) {
      SDL_Quit();
      throw std::runtime_error{std::string("TTF_Init failed: ") + TTF_GetError()};
    }
  }

  ~SdlTtfFixture() {
    TTF_Quit();
    SDL_Quit();
  }
};

class TempFontFile {
 public:
  TempFontFile() {
    const char* tmpdir = std::getenv("TMPDIR");
    if (tmpdir == nullptr || tmpdir[0] == '\0') {
#ifdef _WIN32
      char win_tmp[MAX_PATH];
      if (GetTempPathA(sizeof(win_tmp), win_tmp) == 0) {
        throw std::runtime_error{"GetTempPathA failed"};
      }
      tmpdir = win_tmp;
#else
      tmpdir = "/tmp";
#endif
    }

    path_ = tmpdir;
    if (path_.back() != '/' && path_.back() != '\\') {
      path_ += '/';
    }
    path_ += "vc_font_XXXXXX";

    std::vector<char> buf(path_.begin(), path_.end());
    buf.push_back('\0');

#ifdef _WIN32
    if (_mktemp(buf.data()) == nullptr) {
      throw std::runtime_error{"_mktemp failed"};
    }
    path_ = buf.data();
    fd_ = open(path_.c_str(), O_RDWR | O_CREAT | O_EXCL | O_BINARY, 0600);
#else
    fd_ = mkstemp(buf.data());
    if (fd_ == -1) {
      throw std::runtime_error{"mkstemp failed"};
    }
    path_ = buf.data();
#endif

    if (fd_ == -1) {
      throw std::runtime_error{"failed to open temp font file"};
    }

    const ssize_t written =
#ifdef _WIN32
        _write(fd_, SOURCE_CODE_PRO_REGULAR_TTF, SOURCE_CODE_PRO_REGULAR_TTF_LEN);
#else
        write(fd_, SOURCE_CODE_PRO_REGULAR_TTF, SOURCE_CODE_PRO_REGULAR_TTF_LEN);
#endif
    if (written != static_cast<ssize_t>(SOURCE_CODE_PRO_REGULAR_TTF_LEN)) {
      throw std::runtime_error{"incomplete temp font write"};
    }

#ifdef _WIN32
    _close(fd_);
#else
    close(fd_);
#endif
    fd_ = -1;
  }

  ~TempFontFile() {
    if (!path_.empty()) {
      std::remove(path_.c_str());
    }
  }

  const std::string& path() const { return path_; }

  TempFontFile(const TempFontFile&) = delete;
  TempFontFile& operator=(const TempFontFile&) = delete;

 private:
  std::string path_;
  int fd_{-1};
};

static bool utf8_is_well_formed(TTF_Font* probe, const std::string& text) {
  bool malformed = false;
  font_supports_utf8_text(probe, text, &malformed);
  return !malformed;
}

static void test_utf8_well_formed(TTF_Font* probe) {
  expect(utf8_is_well_formed(probe, "hello"), "ASCII hello");
  expect(utf8_is_well_formed(probe, "Résumé"), "Latin supplement");
  expect(utf8_is_well_formed(probe, "日本語"), "BMP CJK");
  expect(utf8_is_well_formed(probe, "\xF0\x9F\x98\x80"), "U+1F600 supplementary plane");

  expect(!utf8_is_well_formed(probe, "\xFF"), "invalid leading byte");
  expect(!utf8_is_well_formed(probe, "\xC2"), "truncated two-byte sequence");
  expect(!utf8_is_well_formed(probe, "\xE0\xA0"), "truncated three-byte sequence");
  expect(!utf8_is_well_formed(probe, "\xF0\x9F\x98"), "truncated four-byte sequence");
  expect(!utf8_is_well_formed(probe, "\xC2\x28"), "invalid continuation byte");
  expect(!utf8_is_well_formed(probe, "\xC0\xAF"), "overlong encoding");
  expect(!utf8_is_well_formed(probe, "\xED\xA0\x80"), "UTF-16 surrogate encoding");
  expect(!utf8_is_well_formed(probe, "\xF4\x90\x80\x80"), "code point above U+10FFFF");
}

static TTF_Font* open_scp_probe() {
  return open_ui_font_at_size(FontSelection{FontMode::SourceCodePro, {}}, EmbeddedFont::SourceCodePro, 16, "probe");
}

static void test_font_supports_utf8_text(TTF_Font* probe) {
  expect(font_supports_utf8_text(probe, "hello"), "ASCII supported by SCP");
  expect(font_supports_utf8_text(probe, "Résumé"), "two-byte UTF-8 supported by SCP");

  bool malformed = false;
  expect(!font_supports_utf8_text(probe, "bad\xffname.mp4", &malformed), "malformed UTF-8 unsupported");
  expect(malformed, "malformed UTF-8 sets malformed flag");

  malformed = true;
  expect(!font_supports_utf8_text(probe, "普通话视频.mp4", &malformed), "missing CJK glyph unsupported");
  expect(!malformed, "missing glyph does not set malformed flag");

  malformed = false;
  expect(!font_supports_utf8_text(probe, "\xE6\x99\xAE\xFF", &malformed), "unsupported glyph then 0xFF unsupported");
  expect(malformed, "0xFF after missing glyph sets malformed flag");

  bool threw = false;
  try {
    font_supports_utf8_text(nullptr, "hello");
  } catch (const std::logic_error&) {
    threw = true;
  }
  expect(threw, "null font throws logic_error");
}

static void test_forced_modes() {
  expect(embedded_font_for_forced_mode(FontMode::SourceCodePro) == EmbeddedFont::SourceCodePro, "forced scp");
  expect(embedded_font_for_forced_mode(FontMode::Sarasa) == EmbeddedFont::Sarasa, "forced sarasa");

  bool threw = false;
  try {
    embedded_font_for_forced_mode(FontMode::Auto);
  } catch (const std::logic_error&) {
    threw = true;
  }
  expect(threw, "forced mode rejects Auto");

  threw = false;
  try {
    embedded_font_for_forced_mode(FontMode::CustomFile);
  } catch (const std::logic_error&) {
    threw = true;
  }
  expect(threw, "forced mode rejects CustomFile");
}

static void test_format_custom_font_open_error() {
  const std::string path = "/tmp/My Font.ttf";
  expect(format_custom_font_open_error(path, 16, "small text", "Couldn't open /tmp/My Font.ttf: No such file or directory") ==
             "failed to open custom UI font '/tmp/My Font.ttf' for small text (16 pt): No such file or directory",
         "strip duplicated path from common SDL_ttf error");

  expect(format_custom_font_open_error(path, 24, "large text", "Library not initialized") == "failed to open custom UI font '/tmp/My Font.ttf' for large text (24 pt): Library not initialized", "preserve unrelated SDL_ttf error");

  expect(format_custom_font_open_error(path, 16, "small text", nullptr) == "failed to open custom UI font '/tmp/My Font.ttf' for small text (16 pt): unknown error", "null ttf error");

  expect(format_custom_font_open_error(path, 16, "small text", "") == "failed to open custom UI font '/tmp/My Font.ttf' for small text (16 pt): unknown error", "empty ttf error");

  const std::string spaced_path = "/path/with spaces/font";
  expect(format_custom_font_open_error(spaced_path, 16, "small text", "Couldn't open /path/with spaces/font: permission denied") ==
             "failed to open custom UI font '/path/with spaces/font' for small text (16 pt): permission denied",
         "strip duplicated path when path contains spaces");
}

static void expect_open_error_matches(const std::string& path, int point_size, const char* text_role, const std::string& label) {
  bool threw = false;
  try {
    open_custom_font_file(path, point_size, text_role);
  } catch (const std::runtime_error& e) {
    threw = true;
    const std::string msg = e.what();
    expect(msg.find("failed to open custom UI font '" + path + "'") != std::string::npos, (label + ": prefix and path").c_str());
    expect(msg.find("for " + std::string(text_role)) != std::string::npos, (label + ": text role").c_str());
    expect(msg.find("(" + std::to_string(point_size) + " pt)") != std::string::npos, (label + ": point size").c_str());
    expect(msg.find(": ") != std::string::npos, (label + ": reason delimiter").c_str());
    expect(msg.find("Couldn't open " + path) == std::string::npos, (label + ": no duplicated SDL path prefix").c_str());
  }
  expect(threw, (label + ": expected runtime_error").c_str());
}

static void test_open_custom_font_file() {
  TempFontFile temp_font;

  TTF_Font* small = open_custom_font_file(temp_font.path(), 16, "small text");
  expect(small != nullptr, "open custom file at 16 pt");
  TTF_CloseFont(small);

  TTF_Font* large = open_custom_font_file(temp_font.path(), 24, "large text");
  expect(large != nullptr, "open custom file at 24 pt");
  TTF_CloseFont(large);

  expect_open_error_matches("/nonexistent/vc_font_test.ttf", 16, "small text", "missing custom file");
  expect_open_error_matches("sarsa", 16, "small text", "typo path sarsa");
  expect_open_error_matches("/path/with spaces/missing.ttf", 16, "small text", "missing custom file with spaces");
}

static void test_open_ui_font_at_size() {
  TTF_Font* scp = open_ui_font_at_size(FontSelection{FontMode::SourceCodePro, {}}, EmbeddedFont::SourceCodePro, 16, "small text");
  expect(scp != nullptr, "open_ui_font_at_size Source Code Pro");
  TTF_CloseFont(scp);

  TTF_Font* sarasa = open_ui_font_at_size(FontSelection{FontMode::Sarasa, {}}, EmbeddedFont::Sarasa, 16, "small text");
  expect(sarasa != nullptr, "open_ui_font_at_size Sarasa");
  TTF_CloseFont(sarasa);

  TempFontFile temp_font;
  const FontSelection custom{FontMode::CustomFile, temp_font.path()};
  TTF_Font* custom_font = open_ui_font_at_size(custom, EmbeddedFont::Sarasa, 16, "small text");
  expect(custom_font != nullptr, "open_ui_font_at_size custom file ignores active_embedded_font");
  TTF_CloseFont(custom_font);

  bool threw = false;
  try {
    open_ui_font_at_size(FontSelection{FontMode::CustomFile, {}}, EmbeddedFont::SourceCodePro, 16, "small text");
  } catch (const std::logic_error&) {
    threw = true;
  }
  expect(threw, "open_ui_font_at_size rejects empty custom path");

  bool missing_threw = false;
  try {
    open_ui_font_at_size(FontSelection{FontMode::CustomFile, "/nonexistent/vc_font_test.ttf"}, EmbeddedFont::SourceCodePro, 16, "small text");
  } catch (const std::runtime_error& e) {
    missing_threw = true;
    const std::string msg = e.what();
    expect(msg.find("failed to open custom UI font '/nonexistent/vc_font_test.ttf'") != std::string::npos, "open_ui_font_at_size missing custom file message");
    expect(msg.find("Couldn't open /nonexistent/vc_font_test.ttf") == std::string::npos, "open_ui_font_at_size no duplicated path");
  }
  expect(missing_threw, "open_ui_font_at_size missing custom file throws");
}

int main() {
  try {
    SdlTtfFixture fixture;
    (void)fixture;

    test_forced_modes();
    test_format_custom_font_open_error();
    test_open_custom_font_file();
    test_open_ui_font_at_size();

    TTF_Font* probe = open_scp_probe();
    test_utf8_well_formed(probe);
    test_font_supports_utf8_text(probe);

    expect(resolve_auto_embedded_font(probe, "first video.mp4", "second_video_2026.mkv") == EmbeddedFont::SourceCodePro, "auto Latin/Latin -> SCP");
    expect(resolve_auto_embedded_font(probe, "original.mp4", "比較テスト.mp4") == EmbeddedFont::Sarasa, "auto Latin/CJK -> Sarasa");
    expect(resolve_auto_embedded_font(probe, "普通话视频.mp4", "original.mp4") == EmbeddedFont::Sarasa, "auto CJK/Latin -> Sarasa");

    bool malformed = false;
    expect(resolve_auto_embedded_font(probe, "bad\xffname.mp4", "ok.mp4", &malformed) == EmbeddedFont::Sarasa, "auto malformed UTF-8 -> Sarasa");
    expect(malformed, "auto malformed UTF-8 flag");

    malformed = false;
    expect(resolve_auto_embedded_font(probe, "ok.mp4", "bad\xffname.mp4", &malformed) == EmbeddedFont::Sarasa, "auto malformed UTF-8 in right label -> Sarasa");
    expect(malformed, "auto malformed UTF-8 flag from right label");

    TTF_CloseFont(probe);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL unexpected exception: %s\n", e.what());
    return EXIT_FAILURE;
  }

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }

  std::printf("All font selection tests passed\n");
  return EXIT_SUCCESS;
}
