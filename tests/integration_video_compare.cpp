#define SDL_MAIN_HANDLED
#include "keyframe_index.h"
#include "keyframe_index_probe.h"
#include "playback_keyframe.h"
#include "video_compare.h"
extern "C" {
#include <libavformat/avformat.h>
}
#include <SDL2/SDL.h>
#include <limits.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#ifdef _WIN32
#include <stdlib.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <vector>

namespace {

enum class Scenario {
  Baseline,
  EventInjection,
  Seek,
  StillSeek,
  SyncMismatch,
  MultiRightSync,
  FrameNavigation,
  KeyframeNavigation,
  KeyframeNavigationGop25,
  KeyframeNavigationAsymmetric,
  KeyframeNavigationUnionTimeShift,
  BufferForwardOnly,
  BufferPingPong,
  SeekBurstForward,
  SeekBurstMixed,
  CropCopy,
  InteractiveCrop,
  DynamicSwap
};

std::atomic<int> events_pushed{0};
std::atomic<bool> watchdog_fired{false};

struct IntegrationStdoutCapture {
  mutable std::mutex mutex;
  std::string data;

  class StreamBuf final : public std::streambuf {
   public:
    explicit StreamBuf(IntegrationStdoutCapture* owner) : owner_(owner) {}

   protected:
    int overflow(int character) override {
      if (character == EOF) {
        return EOF;
      }
      append_character(static_cast<char>(character));
      return character;
    }

    std::streamsize xsputn(const char* characters, std::streamsize count) override {
      if (count > 0) {
        std::lock_guard<std::mutex> lock(owner_->mutex);
        owner_->data.append(characters, static_cast<size_t>(count));
      }
      return count;
    }

   private:
    IntegrationStdoutCapture* owner_{nullptr};

    void append_character(const char character) {
      std::lock_guard<std::mutex> lock(owner_->mutex);
      owner_->data.push_back(character);
    }
  };

  StreamBuf stream_buf;

  IntegrationStdoutCapture() : stream_buf(this) {}

  std::string snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    return data;
  }
};

IntegrationStdoutCapture* g_integration_stdout_capture = nullptr;
std::vector<size_t>* g_official_display_dump_indices = nullptr;

struct CropCopyCheckpointFailure {
  std::atomic<bool> failed{false};
  std::mutex message_mutex;
  std::string message;
};

CropCopyCheckpointFailure g_crop_copy_checkpoint;

void reset_helper_counters() {
  events_pushed.store(0);
  watchdog_fired.store(false);
}

bool is_stress_scenario(const Scenario scenario) {
  return scenario == Scenario::SeekBurstForward || scenario == Scenario::SeekBurstMixed;
}

const char* scenario_name(const Scenario scenario) {
  switch (scenario) {
    case Scenario::Baseline:
      return "baseline";
    case Scenario::EventInjection:
      return "event-injection";
    case Scenario::Seek:
      return "seek";
    case Scenario::StillSeek:
      return "still-seek";
    case Scenario::SyncMismatch:
      return "sync-mismatch";
    case Scenario::MultiRightSync:
      return "multi-right-sync";
    case Scenario::FrameNavigation:
      return "frame-navigation";
    case Scenario::KeyframeNavigation:
      return "keyframe-navigation";
    case Scenario::KeyframeNavigationGop25:
      return "keyframe-navigation-gop25";
    case Scenario::KeyframeNavigationAsymmetric:
      return "keyframe-navigation-asymmetric";
    case Scenario::KeyframeNavigationUnionTimeShift:
      return "keyframe-navigation-union-time-shift";
    case Scenario::BufferForwardOnly:
      return "buffer-forward-only";
    case Scenario::BufferPingPong:
      return "buffer-pingpong";
    case Scenario::SeekBurstForward:
      return "seek-burst-forward";
    case Scenario::SeekBurstMixed:
      return "seek-burst-mixed";
    case Scenario::CropCopy:
      return "crop-copy";
    case Scenario::InteractiveCrop:
      return "interactive-crop";
    case Scenario::DynamicSwap:
      return "dynamic-swap";
  }
  return "unknown";
}

const char* stress_iteration_label() {
  const char* value = std::getenv("STRESS_ITERATION");
  return (value != nullptr && value[0] != '\0') ? value : "?";
}

int watchdog_ticks_for(const Scenario scenario) {
  // 100 ms per tick. Stress seeks serialize on drain/restart; 12 s is too tight.
  // crop-copy walks several crop/copy/undo/Shift+X rounds after the shared settle.
  if (scenario == Scenario::CropCopy) {
    return 400;
  }
  if (scenario == Scenario::InteractiveCrop) {
    return 250;
  }
  if (scenario == Scenario::KeyframeNavigationUnionTimeShift) {
    const char* steps_env = std::getenv("KEYFRAME_NAV_STEPS");
    const char* warmup_env = std::getenv("KEYFRAME_NAV_WARMUP_D");
    const int steps = (steps_env != nullptr && steps_env[0] != '\0') ? std::max(1, std::atoi(steps_env)) : 3;
    const int warmup = (warmup_env != nullptr && warmup_env[0] != '\0') ? std::max(0, std::atoi(warmup_env)) : 0;
    return 120 + (warmup + steps + 1) * 28;
  }
  return is_stress_scenario(scenario) ? 250 : 120;
}

void sleep_ms(const int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void require_readable_file(const char* path) {
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    throw std::runtime_error(std::string("fixture is not readable: ") + path);
  }
  std::fclose(file);
}

void prepare_dummy_software_sdl() {
  SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv("SDL_RENDER_DRIVER", "software", 1);
}

Uint32 first_sdl_window_id() {
  for (Uint32 id = 1; id < 32; ++id) {
    if (SDL_GetWindowFromID(id) != nullptr) {
      return id;
    }
  }
  return 0;
}

void push_quit() {
  SDL_Event event{};
  event.type = SDL_QUIT;
  if (SDL_PushEvent(&event) < 0) {
    std::fprintf(stderr, "SDL_PushEvent(SDL_QUIT) failed: %s\n", SDL_GetError());
  } else {
    events_pushed.fetch_add(1);
  }
}

void push_keydown(const SDL_Keycode key, const SDL_Scancode scancode, const Uint32 window_id, const SDL_Keymod mod = KMOD_NONE) {
  SDL_Event event{};
  event.type = SDL_KEYDOWN;
  event.key.type = SDL_KEYDOWN;
  event.key.timestamp = SDL_GetTicks();
  event.key.windowID = window_id;
  event.key.state = SDL_PRESSED;
  event.key.repeat = 0;
  event.key.keysym.scancode = scancode;
  event.key.keysym.sym = key;
  event.key.keysym.mod = mod;
  if (SDL_PushEvent(&event) < 0) {
    std::fprintf(stderr, "SDL_PushEvent(KEYDOWN) failed: %s\n", SDL_GetError());
  } else {
    events_pushed.fetch_add(1);
  }
}

SDL_Keymod clipboard_mod() {
#ifdef __APPLE__
  return static_cast<SDL_Keymod>(KMOD_GUI);
#else
  return static_cast<SDL_Keymod>(KMOD_CTRL);
#endif
}

void push_copy_timestamp() {
  push_keydown(SDLK_c, SDL_SCANCODE_C, 0, clipboard_mod());
}

void push_crop_gesture(const SDL_Keycode side_key, const SDL_Scancode side_scancode, const int x0, const int y0, const int x1, const int y1) {
  SDL_Event events[5]{};
  const Uint32 timestamp = SDL_GetTicks();

  events[0].type = SDL_KEYDOWN;
  events[0].key.type = SDL_KEYDOWN;
  events[0].key.timestamp = timestamp;
  events[0].key.windowID = 0;
  events[0].key.state = SDL_PRESSED;
  events[0].key.repeat = 0;
  events[0].key.keysym.scancode = side_scancode;
  events[0].key.keysym.sym = side_key;
  events[0].key.keysym.mod = KMOD_SHIFT;

  events[1].type = SDL_MOUSEMOTION;
  events[1].motion.type = SDL_MOUSEMOTION;
  events[1].motion.timestamp = timestamp;
  events[1].motion.windowID = 0;
  events[1].motion.x = x0;
  events[1].motion.y = y0;

  events[2].type = SDL_MOUSEBUTTONDOWN;
  events[2].button.type = SDL_MOUSEBUTTONDOWN;
  events[2].button.timestamp = timestamp;
  events[2].button.windowID = 0;
  events[2].button.button = SDL_BUTTON_LEFT;
  events[2].button.state = SDL_PRESSED;
  events[2].button.clicks = 1;
  events[2].button.x = x0;
  events[2].button.y = y0;

  events[3].type = SDL_MOUSEMOTION;
  events[3].motion.type = SDL_MOUSEMOTION;
  events[3].motion.timestamp = timestamp;
  events[3].motion.windowID = 0;
  events[3].motion.x = x1;
  events[3].motion.y = y1;

  events[4].type = SDL_MOUSEBUTTONUP;
  events[4].button.type = SDL_MOUSEBUTTONUP;
  events[4].button.timestamp = timestamp;
  events[4].button.windowID = 0;
  events[4].button.button = SDL_BUTTON_LEFT;
  events[4].button.state = SDL_RELEASED;
  events[4].button.clicks = 1;
  events[4].button.x = x1;
  events[4].button.y = y1;

  constexpr int kEventCount = sizeof(events) / sizeof(events[0]);
  const int added = SDL_PeepEvents(events, kEventCount, SDL_ADDEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT);
  if (added < 0) {
    std::fprintf(stderr, "SDL_PeepEvents(crop gesture) failed: %s\n", SDL_GetError());
    return;
  }

  events_pushed.fetch_add(added);
  if (added != kEventCount) {
    std::fprintf(stderr, "SDL_PeepEvents(crop gesture) added %d/%d events\n", added, kEventCount);
  }
}

void wait_for_filter_refresh() {
  // Crop/copy/clear/undo force a same-position seek and filter rebuild. Shift+X
  // reads Display metadata, which updates only after the new frames refresh.
  sleep_ms(700);
}

size_t count_display_state_messages(const std::string& output) {
  size_t count = 0;
  size_t pos = 0;
  const std::string needle = "Display state:";
  while ((pos = output.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

constexpr size_t kInvalidDisplayDumpIndex = static_cast<size_t>(-1);

size_t display_state_count_locked() {
  if (g_integration_stdout_capture == nullptr) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_integration_stdout_capture->mutex);
  return count_display_state_messages(g_integration_stdout_capture->data);
}

// Emit Shift+X and return the 0-based index of a newly appended Display state line.
bool emit_shift_x_and_wait_for_new_display_state(size_t* new_line_index, const int response_wait_ms) {
  const size_t before_count = display_state_count_locked();
  push_keydown(SDLK_x, SDL_SCANCODE_X, 0, KMOD_SHIFT);

  constexpr int kPollStepMs = 25;
  for (int waited = 0; waited < response_wait_ms; waited += kPollStepMs) {
    sleep_ms(kPollStepMs);
    const size_t after_count = display_state_count_locked();
    if (after_count > before_count) {
      *new_line_index = before_count;
      return true;
    }
  }
  return false;
}

void dump_display_filters() {
  if (g_official_display_dump_indices == nullptr) {
    push_keydown(SDLK_x, SDL_SCANCODE_X, 0, KMOD_SHIFT);
    sleep_ms(200);
    return;
  }

  size_t new_line_index = 0;
  constexpr int kOfficialDumpResponseWaitMs = 2500;
  if (emit_shift_x_and_wait_for_new_display_state(&new_line_index, kOfficialDumpResponseWaitMs)) {
    g_official_display_dump_indices->push_back(new_line_index);
  } else {
    g_official_display_dump_indices->push_back(kInvalidDisplayDumpIndex);
  }
}

void select_right_video(const int index) {
  push_keydown(static_cast<SDL_Keycode>(SDLK_1 + index), static_cast<SDL_Scancode>(SDL_SCANCODE_1 + index), 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
  sleep_ms(300);
}

void toggle_swap() {
  push_keydown(SDLK_s, SDL_SCANCODE_S, 0);
  sleep_ms(200);
}

void interactive_crop(const SDL_Keycode side_key, const SDL_Scancode side_scancode, const int x0, const int y0, const int x1, const int y1) {
  // Queue crop-mode entry and the complete drag as one transaction. This keeps
  // the synthetic gesture contiguous even if the platform pumps native mouse
  // events between main-loop iterations.
  push_crop_gesture(side_key, side_scancode, x0, y0, x1, y1);
  wait_for_filter_refresh();
}

void copy_visual_left_crop_to_others() {
  // Ctrl+Shift+L: visual-left crop -> right-side video(s). After Swap that is only the visual right.
  push_keydown(SDLK_l, SDL_SCANCODE_L, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
  wait_for_filter_refresh();
}

void copy_visual_right_crop_to_left() {
  push_keydown(SDLK_r, SDL_SCANCODE_R, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
  wait_for_filter_refresh();
}

void clear_visual_left_crop() {
  // Ctrl+L: clear the video currently displayed on the left.
  push_keydown(SDLK_l, SDL_SCANCODE_L, 0, KMOD_CTRL);
  wait_for_filter_refresh();
}

void clear_visual_right_crop() {
  // Ctrl+R: clear the video currently displayed on the right.
  push_keydown(SDLK_r, SDL_SCANCODE_R, 0, KMOD_CTRL);
  wait_for_filter_refresh();
}

void undo_last_crop() {
  push_keydown(SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE, 0);
  wait_for_filter_refresh();
}

std::string extract_quoted_field(const std::string& line, const std::string& key) {
  const std::string prefix = key + "=\"";
  const size_t start = line.find(prefix);
  if (start == std::string::npos) {
    return std::string();
  }
  std::string value;
  for (size_t i = start + prefix.size(); i < line.size(); ++i) {
    if (line[i] == '\\' && i + 1 < line.size()) {
      value.push_back(line[i + 1]);
      ++i;
      continue;
    }
    if (line[i] == '"') {
      break;
    }
    value.push_back(line[i]);
  }
  return value;
}

std::string extract_unquoted_field(const std::string& line, const std::string& key) {
  const std::string token = " " + key + "=";
  const size_t start = line.find(token);
  if (start == std::string::npos) {
    return std::string();
  }
  const size_t value_start = start + token.size();
  const size_t value_end = line.find(' ', value_start);
  return line.substr(value_start, value_end == std::string::npos ? std::string::npos : value_end - value_start);
}

struct FilterDump {
  std::string left;
  std::string right;
  std::string swapped;
  std::string right_id;
  std::string visual_left;
  std::string visual_right;
  std::string aspect;
  std::string window;
  std::string content;
  std::string ref_dar;
};

std::vector<FilterDump> parse_display_filters(const std::string& output) {
  std::vector<FilterDump> dumps;
  std::istringstream in(output);
  std::string line;
  while (std::getline(in, line)) {
    if (line.find("Display state:") == std::string::npos) {
      continue;
    }
    FilterDump dump;
    dump.left = extract_quoted_field(line, "filters_left");
    dump.right = extract_quoted_field(line, "filters_right");
    dump.swapped = extract_unquoted_field(line, "swapped");
    dump.right_id = extract_unquoted_field(line, "right");
    dump.visual_left = extract_unquoted_field(line, "visual_left");
    dump.visual_right = extract_unquoted_field(line, "visual_right");
    dump.aspect = extract_unquoted_field(line, "aspect");
    dump.window = extract_unquoted_field(line, "window");
    dump.content = extract_unquoted_field(line, "content");
    dump.ref_dar = extract_unquoted_field(line, "ref_dar");
    dumps.push_back(dump);
  }
  return dumps;
}

CropState crop_state_from_token(const std::string& token) {
  CropState state;
  int width = 0;
  int height = 0;
  int x = 0;
  int y = 0;
  if (std::sscanf(token.c_str(), "crop=%d:%d:%d:%d", &width, &height, &x, &y) == 4) {
    state.enabled = true;
    state.rect = {x, y, width, height};
  }
  return state;
}

std::string mapped_crop_token(const std::string& source_token, const int source_w, const int source_h, const int dest_w, const int dest_h) {
  const CropState mapped = video_filter_state::map_crop_state(crop_state_from_token(source_token), source_w, source_h, dest_w, dest_h);
  return mapped.enabled ? video_filter_state::crop_filter(mapped.rect) : std::string();
}

struct WindowSize {
  int w{0};
  int h{0};
};

struct ContentViewport {
  int x{0};
  int y{0};
  int w{0};
  int h{0};
};

bool same_viewport(const ContentViewport& a, const ContentViewport& b) {
  return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool parse_window_size(const std::string& text, WindowSize* size) {
  return std::sscanf(text.c_str(), "%dx%d", &size->w, &size->h) == 2 && size->w > 0 && size->h > 0;
}

bool parse_content_viewport(const std::string& text, ContentViewport* viewport) {
  return std::sscanf(text.c_str(), "%d,%d,%dx%d", &viewport->x, &viewport->y, &viewport->w, &viewport->h) == 4 && viewport->w > 0 && viewport->h > 0;
}

bool parse_dar(const std::string& text, int* num, int* den) {
  return std::sscanf(text.c_str(), "%d:%d", num, den) == 2 && *num > 0 && *den > 0;
}

bool dar_equals(const int num, const int den, const int want_num, const int want_den) {
  return den != 0 && want_den != 0 && num * want_den == den * want_num;
}

// Same rounding as Display::update_content_window_layout() for one frame aspect.
ContentViewport viewport_for_aspect(const WindowSize& window, const float content_aspect) {
  ContentViewport viewport;
  const float window_aspect = static_cast<float>(window.w) / static_cast<float>(window.h);
  if (window_aspect > content_aspect) {
    viewport.h = window.h;
    const int rounded = static_cast<int>(std::round(static_cast<float>(viewport.h) * content_aspect));
    viewport.w = rounded < 1 ? 1 : rounded;
    viewport.x = (window.w - viewport.w) / 2;
  } else {
    viewport.w = window.w;
    const int rounded = static_cast<int>(std::round(static_cast<float>(viewport.w) / content_aspect));
    viewport.h = rounded < 1 ? 1 : rounded;
    viewport.y = (window.h - viewport.h) / 2;
  }
  return viewport;
}

std::string crop_token(const std::string& filters) {
  const size_t start = filters.find("crop=");
  if (start == std::string::npos) {
    return std::string();
  }
  size_t end = start;
  while (end < filters.size() && filters[end] != ',' && filters[end] != '"') {
    ++end;
  }
  return filters.substr(start, end - start);
}

void print_filter_dumps(const std::vector<FilterDump>& dumps) {
  for (size_t i = 0; i < dumps.size(); ++i) {
    std::printf("crop-copy dump %zu: swapped=%s right=%s visual_left=%s visual_right=%s left=\"%s\" right_filters=\"%s\" crop_left=%s crop_right=%s\n", i, dumps[i].swapped.c_str(), dumps[i].right_id.c_str(), dumps[i].visual_left.c_str(),
                dumps[i].visual_right.c_str(), dumps[i].left.c_str(), dumps[i].right.c_str(), crop_token(dumps[i].left).c_str(), crop_token(dumps[i].right).empty() ? "(none)" : crop_token(dumps[i].right).c_str());
  }
}

bool crop_copy_checkpoint_display_state(const FilterDump& dump, const std::string& crop_a_token) {
  return dump.swapped == "false" && dump.right_id == "2" && crop_token(dump.left) == crop_a_token && crop_token(dump.right).empty();
}

bool wait_for_crop_copy_post_ctrl_r_clear_state() {
  if (g_integration_stdout_capture == nullptr || g_official_display_dump_indices == nullptr || g_official_display_dump_indices->empty()) {
    g_crop_copy_checkpoint.failed.store(true);
    std::lock_guard<std::mutex> lock(g_crop_copy_checkpoint.message_mutex);
    g_crop_copy_checkpoint.message = "undo+Ctrl+R checkpoint missing stdout capture or crop A reference dump";
    return false;
  }

  const std::string output = g_integration_stdout_capture->snapshot();
  const std::vector<FilterDump> all_dumps = parse_display_filters(output);
  const size_t crop_a_dump_index = g_official_display_dump_indices->front();
  if (crop_a_dump_index >= all_dumps.size()) {
    g_crop_copy_checkpoint.failed.store(true);
    std::lock_guard<std::mutex> lock(g_crop_copy_checkpoint.message_mutex);
    g_crop_copy_checkpoint.message = "undo+Ctrl+R checkpoint could not read crop A from official dump 0";
    return false;
  }
  const std::string crop_a_token = crop_token(all_dumps[crop_a_dump_index].left);
  if (crop_a_token.empty()) {
    g_crop_copy_checkpoint.failed.store(true);
    std::lock_guard<std::mutex> lock(g_crop_copy_checkpoint.message_mutex);
    g_crop_copy_checkpoint.message = "undo+Ctrl+R checkpoint crop A token missing from official dump 0";
    return false;
  }

  constexpr int kMaxProbes = 40;
  constexpr int kProbeResponseWaitMs = 400;

  FilterDump last_new_line_dump;
  bool saw_new_line = false;

  for (int probe = 0; probe < kMaxProbes; ++probe) {
    size_t new_line_index = 0;
    if (!emit_shift_x_and_wait_for_new_display_state(&new_line_index, kProbeResponseWaitMs)) {
      continue;
    }

    const std::vector<FilterDump> dumps = parse_display_filters(g_integration_stdout_capture->snapshot());
    if (new_line_index >= dumps.size()) {
      continue;
    }

    const FilterDump& probe_dump = dumps[new_line_index];
    last_new_line_dump = probe_dump;
    saw_new_line = true;
    if (crop_copy_checkpoint_display_state(probe_dump, crop_a_token)) {
      return true;
    }
  }

  std::ostringstream message;
  message << "timed out waiting for observable Shift+X state after undo+Ctrl+R (expected swapped=false right=2 left=" << crop_a_token << " right cleared)";
  if (saw_new_line) {
    message << "; last probe left crop=" << (crop_token(last_new_line_dump.left).empty() ? "(none)" : crop_token(last_new_line_dump.left))
            << " right crop=" << (crop_token(last_new_line_dump.right).empty() ? "(none)" : crop_token(last_new_line_dump.right)) << " swapped=" << last_new_line_dump.swapped << " right=" << last_new_line_dump.right_id;
  } else {
    message << "; Shift+X probes did not produce any new Display state lines";
  }
  g_crop_copy_checkpoint.failed.store(true);
  std::lock_guard<std::mutex> lock(g_crop_copy_checkpoint.message_mutex);
  g_crop_copy_checkpoint.message = message.str();
  return false;
}

bool parse_hhmmss(const std::string& text, double* seconds) {
  int hours = 0;
  int minutes = 0;
  int secs = 0;
  int millis = 0;
  if (std::sscanf(text.c_str(), "%d:%d:%d.%d", &hours, &minutes, &secs, &millis) != 4) {
    return false;
  }
  *seconds = static_cast<double>(hours * 3600 + minutes * 60 + secs) + static_cast<double>(millis) / 1000.0;
  return true;
}

bool file_is_non_empty(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

std::string resolve_path(const std::string& path) {
#if defined(_WIN32)
  char resolved[_MAX_PATH];
  if (::_fullpath(resolved, path.c_str(), sizeof(resolved)) == nullptr) {
    throw std::runtime_error("cannot resolve path: " + path);
  }
#else
  char resolved[PATH_MAX];
  if (::realpath(path.c_str(), resolved) == nullptr) {
    throw std::runtime_error("cannot resolve path: " + path);
  }
#endif
  return resolved;
}

// F writes PNGs into cwd. Isolate still-seek dumps so the repo root stays clean.
struct DumpDirectory {
  std::string previous;
  std::string path;

  explicit DumpDirectory(const char* prefix) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) {
      throw std::runtime_error("getcwd failed");
    }
    previous = cwd;

    const char* tmp = std::getenv("TMPDIR");
    if (tmp == nullptr || tmp[0] == '\0') {
      tmp = std::getenv("TEMP");
    }
    if (tmp == nullptr || tmp[0] == '\0') {
      tmp = "/tmp";
    }

    std::string tmpl = std::string(tmp) + "/" + prefix + "XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (mkdtemp(buf.data()) == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path = buf.data();
    if (::chdir(path.c_str()) != 0) {
      throw std::runtime_error("chdir to dump directory failed");
    }
  }

  ~DumpDirectory() {
    const char* names[] = {"screenshot_1_0001.png", "screenshot_2_0001.png", "screenshot_1_screenshot_2_osd_0001.png"};
    for (const char* name : names) {
      unlink((path + "/" + name).c_str());
    }
    if (::chdir(previous.c_str()) != 0) {
      std::fprintf(stderr, "warning: failed to restore working directory to %s\n", previous.c_str());
    }
    rmdir(path.c_str());
  }
};

void print_pts_sequence(const char* label, const std::vector<double>& times) {
  std::fprintf(stderr, "%s", label);
  for (size_t i = 0; i < times.size(); ++i) {
    std::fprintf(stderr, "%s%.3f", i == 0 ? "" : " ", times[i]);
  }
  std::fprintf(stderr, "\n");
}

std::vector<double> unique_pts(const std::vector<double>& times) {
  std::vector<double> out;
  for (const double t : times) {
    if (out.empty() || std::fabs(t - out.back()) > 0.015) {
      out.push_back(t);
    }
  }
  return out;
}

bool pts_near(const double a, const double b) {
  return std::fabs(a - b) <= 0.015;
}

constexpr double kUnion48DenseKeyframeStep = 50.0 / 48.0;
constexpr double kUnion48SparseKeyframe = 5.208333;
constexpr double kUnionLatticeMatchTol = 0.022;
constexpr double kUnionStepCursorTol = 0.002;
constexpr double kUnionLatticeDedupS = 0.002;
constexpr double kUnionTol = 0.022;
constexpr double kUnionMinForwardStepS = 0.005;
constexpr double kUnionMaxForwardStepS = 1.12;

bool path_contains_basename(const std::string& path, const char* basename) {
  return path.find(basename) != std::string::npos;
}

std::vector<double> dedupe_sorted_union_lattice(std::vector<double> keys) {
  std::sort(keys.begin(), keys.end());
  std::vector<double> out;
  for (const double key : keys) {
    if (key < -0.001) {
      continue;
    }
    if (out.empty() || key - out.back() > kUnionLatticeDedupS) {
      out.push_back(key);
    }
  }
  return out;
}

std::vector<double> synthetic_union_lattice_dense_left_sparse_right(const double shift_s) {
  std::vector<double> keys;
  for (int n = 0; n <= 10; ++n) {
    keys.push_back(static_cast<double>(n) * kUnion48DenseKeyframeStep);
  }
  keys.push_back(kUnion48SparseKeyframe - shift_s);
  return dedupe_sorted_union_lattice(std::move(keys));
}

std::vector<double> synthetic_union_lattice_sparse_left_dense_right(const double shift_s) {
  std::vector<double> keys = {0.0, kUnion48SparseKeyframe};
  for (int n = 0; n <= 10; ++n) {
    keys.push_back(static_cast<double>(n) * kUnion48DenseKeyframeStep - shift_s);
  }
  return dedupe_sorted_union_lattice(std::move(keys));
}

bool union_lattice_forward_successor(const std::vector<double>& lattice, const double previous, double* successor) {
  for (const double key : lattice) {
    if (key > previous + kUnionStepCursorTol) {
      *successor = key;
      return true;
    }
  }
  return false;
}

bool near_union_lattice(const double observed, const double expected) {
  return std::fabs(observed - expected) <= kUnionLatticeMatchTol;
}

void print_pts_stdout(const char* label, const std::vector<double>& times) {
  std::printf("%s", label);
  for (size_t i = 0; i < times.size(); ++i) {
    std::printf("%s%.3f", i == 0 ? "" : " ", times[i]);
  }
  std::printf("\n");
}

std::vector<double> parse_timestamp_prefixes(const std::string& output, const char* prefix) {
  std::vector<double> times;
  const std::string needle(prefix);
  size_t pos = 0;
  while ((pos = output.find(needle, pos)) != std::string::npos) {
    pos += needle.size();
    const size_t end = output.find_first_of("\r\n", pos);
    const std::string token = output.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    double seconds = 0.0;
    if (parse_hhmmss(token, &seconds)) {
      times.push_back(seconds);
    }
  }
  return times;
}

std::vector<double> copied_positions(const std::string& output) {
  return parse_timestamp_prefixes(output, "Copied to clipboard: ");
}

// The decoder's landing verification already checks the owner demuxer PTS.
// Check the observed comparison timeline independently, not the requested target.
bool append_verified_union_landings(std::vector<double>& times, const std::vector<playback_keyframe::KeyframeNavigationLandingObservation>& landings) {
  for (size_t i = 0; i < landings.size(); ++i) {
    const auto& landing = landings[i];
    if (landing.verification_status != playback_keyframe::KeyframeNavigationStatus::None || !landing.actual_owner_demuxer_valid || !landing.actual_comparison_timeline_valid ||
        std::fabs(landing.actual_comparison_timeline_seconds - landing.requested_comparison_timeline_seconds) > kUnionTol) {
      std::fprintf(stderr, "FAIL union-time-shift landing %zu owner=%s right=%zu status=%d owner_demuxer=%.3f expected=%.3f comparison=%.3f expected=%.3f valid=%d/%d\n", i,
                   landing.candidate_owner == SideType::Left ? "left" : "right", landing.active_right_index, static_cast<int>(landing.verification_status), landing.actual_owner_demuxer_seconds,
                   landing.requested_owner_demuxer_seconds, landing.actual_comparison_timeline_seconds, landing.requested_comparison_timeline_seconds,
                   landing.actual_owner_demuxer_valid, landing.actual_comparison_timeline_valid);
      return false;
    }
    times.push_back(landing.actual_comparison_timeline_seconds);
  }
  return true;
}

void push_relative_seek(const int direction) {
  if (direction > 0) {
    push_keydown(SDLK_RIGHT, SDL_SCANCODE_RIGHT, 0);
  } else {
    push_keydown(SDLK_LEFT, SDL_SCANCODE_LEFT, 0);
  }
}

void print_stress_failure(const Scenario scenario, const char* message, const std::vector<double>& times, const double expected_storm) {
  std::fprintf(stderr, "FAIL %s iteration=%s events_pushed=%d watchdog=%s", scenario_name(scenario), stress_iteration_label(), events_pushed.load(), watchdog_fired.load() ? "fired" : "idle");
  if (times.size() > 0) {
    std::fprintf(stderr, " T0=%.3f", times[0]);
  }
  if (times.size() > 1) {
    std::fprintf(stderr, " T1=%.3f storm=%.3f expected_storm=%.3f", times[1], times[1] - times[0], expected_storm);
  }
  if (times.size() > 2) {
    std::fprintf(stderr, " T2=%.3f T2-T1=%.3f", times[2], times[2] - times[1]);
  }
  if (!times.empty()) {
    std::fprintf(stderr, " last_pts=%.3f", times.back());
  }
  std::fprintf(stderr, " %s\n", message);
}

void run_event_script(const Scenario scenario, std::atomic<bool>& finished) {
  const int watchdog_ticks = watchdog_ticks_for(scenario);
  std::thread watchdog([scenario, watchdog_ticks, &finished]() {
    for (int i = 0; i < watchdog_ticks && !finished.load(); ++i) {
      sleep_ms(100);
    }
    if (!finished.load()) {
      watchdog_fired.store(true);
      std::fprintf(stderr, "watchdog: pushing SDL_QUIT after %ds deadline (%s iteration=%s)\n", watchdog_ticks / 10, scenario_name(scenario), stress_iteration_label());
      push_quit();
    }
  });

  // Construction already created Display/SDL. operator() starts workers
  // immediately after this thread is launched; wait for first frames.
  sleep_ms(1000);

  if (scenario == Scenario::EventInjection) {
    const Uint32 existing_window_id = first_sdl_window_id();
    // Display::handle_event does not filter KEYDOWN by windowID. ScopeManager
    // only consumes events with a non-zero id that matches an open scope.
    // windowID 0 is the case we need to prove is accepted.
    std::fprintf(stderr, "event-injection: first SDL window id=%u; pushing Tab with windowID=0\n", existing_window_id);
    push_keydown(SDLK_TAB, SDL_SCANCODE_TAB, 0);
    sleep_ms(500);
  } else if (scenario == Scenario::Seek) {
    // Pause so the copied PTS is a stable pre-seek landing, not a moving playhead.
    // The seek transaction itself does not consult play_; pause only freezes fetch.
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(400);
    push_copy_timestamp();
    sleep_ms(300);
    // RIGHT/LEFT are relative ±1.0s (KMOD_NONE). begin_input_frame clears
    // seek_from_start, so these are not timeline-from-start seeks.
    push_keydown(SDLK_RIGHT, SDL_SCANCODE_RIGHT, 0);
    sleep_ms(700);
    push_copy_timestamp();
    sleep_ms(300);
    push_keydown(SDLK_LEFT, SDL_SCANCODE_LEFT, 0);
    sleep_ms(700);
    push_copy_timestamp();
    sleep_ms(300);
    // Ordinary forward seeks past EOF: advance near clip end, then one more +1s
    // should fail; demuxers restore and the displayed left PTS stays stable.
    for (int i = 0; i < 3; ++i) {
      push_keydown(SDLK_RIGHT, SDL_SCANCODE_RIGHT, 0);
      sleep_ms(450);
    }
    push_copy_timestamp();
    sleep_ms(300);
    push_keydown(SDLK_RIGHT, SDL_SCANCODE_RIGHT, 0);
    sleep_ms(700);
    push_copy_timestamp();
    sleep_ms(300);
  } else if (scenario == Scenario::StillSeek) {
    // JPEG/image2 becomes SingleFrame after the first unique PTS, then EOF.
    // The 1000 ms settle above is required: a JPEG input alone does not mean
    // single_frame is already true when the first event arrives.
    // RIGHT only: plan_seek rewrites seek_relative from left.delta_pts
    // before the backward decision, so LEFT (-1.0) becomes the same
    // forward still-seek when delta_pts > 0.
    push_keydown(SDLK_RIGHT, SDL_SCANCODE_RIGHT, 0);
    sleep_ms(600);
    // Proof of life after skip_update + buffer restore. Do not quit
    // immediately after the seek. F dumps the presented stills.
    push_keydown(SDLK_f, SDL_SCANCODE_F, 0);
    sleep_ms(500);
  } else if (scenario == Scenario::SyncMismatch) {
    // Natural playback only. The shared 1000 ms settle plus this extra wait
    // give 25-vs-30 enough dual-pops for right to cross min_delta repeatedly.
    sleep_ms(800);
    push_copy_timestamp();
    sleep_ms(300);
  } else if (scenario == Scenario::MultiRightSync) {
    // Unpaused 30/25/25 playback. Tab is after the sync window so it does
    // not change active-right tolerance while the repeated-left-pop path runs.
    sleep_ms(1000);
    push_copy_timestamp();
    sleep_ms(200);
    push_keydown(SDLK_TAB, SDL_SCANCODE_TAB, 0);
    sleep_ms(200);
  } else if (scenario == Scenario::FrameNavigation) {
    // Pause isolates one-frame movement from continuous fetch. SPACE
    // toggles play_ to false; Shift+D still fetches via
    // forward_navigate_frames, and Shift+A still enters the seek path.
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(300);
    push_copy_timestamp();
    sleep_ms(200);
    // Shift only — do not combine with the clipboard modifier.
    push_keydown(SDLK_d, SDL_SCANCODE_D, 0, KMOD_SHIFT);
    sleep_ms(500);
    push_copy_timestamp();
    sleep_ms(200);
    push_keydown(SDLK_a, SDL_SCANCODE_A, 0, KMOD_SHIFT);
    sleep_ms(700);
    push_copy_timestamp();
    sleep_ms(200);
  } else if (scenario == Scenario::KeyframeNavigation) {
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(300);
    push_keydown(SDLK_LEFT, SDL_SCANCODE_LEFT, 0);
    sleep_ms(700);
    push_copy_timestamp();
    sleep_ms(200);
    push_keydown(SDLK_d, SDL_SCANCODE_D, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
    sleep_ms(900);
    push_copy_timestamp();
    sleep_ms(200);
    // No next keyframe at the end of the two-keyframe fixture.
    push_keydown(SDLK_d, SDL_SCANCODE_D, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
    sleep_ms(900);
    push_copy_timestamp();
    sleep_ms(200);
    push_keydown(SDLK_a, SDL_SCANCODE_A, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
    sleep_ms(900);
    push_copy_timestamp();
    sleep_ms(200);
    // No previous keyframe at the beginning either.
    push_keydown(SDLK_a, SDL_SCANCODE_A, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
    sleep_ms(900);
    push_copy_timestamp();
    sleep_ms(200);
    // Seek to the final keyframe, play into its GOP, then request a next
    // keyframe that does not exist. Rejected navigation must not rewind.
    push_keydown(SDLK_d, SDL_SCANCODE_D, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
    sleep_ms(900);
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(1000);
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(300);
    push_copy_timestamp();
    sleep_ms(200);
    push_keydown(SDLK_d, SDL_SCANCODE_D, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
    sleep_ms(900);
    push_copy_timestamp();
    sleep_ms(200);
  } else if (scenario == Scenario::KeyframeNavigationGop25) {
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(300);
    push_keydown(SDLK_RIGHT, SDL_SCANCODE_RIGHT, 0, KMOD_SHIFT);
    sleep_ms(700);
    push_copy_timestamp();
    sleep_ms(200);
    push_keydown(SDLK_d, SDL_SCANCODE_D, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
    sleep_ms(700);
    push_copy_timestamp();
    sleep_ms(200);
    push_keydown(SDLK_d, SDL_SCANCODE_D, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
    sleep_ms(700);
    push_copy_timestamp();
    sleep_ms(200);
    push_keydown(SDLK_a, SDL_SCANCODE_A, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
    sleep_ms(700);
    push_copy_timestamp();
    sleep_ms(200);
  } else if (scenario == Scenario::KeyframeNavigationAsymmetric) {
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(300);
    push_keydown(SDLK_RIGHT, SDL_SCANCODE_RIGHT, 0, KMOD_SHIFT);
    sleep_ms(700);
    push_copy_timestamp();
    sleep_ms(200);
    push_keydown(SDLK_d, SDL_SCANCODE_D, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
    sleep_ms(700);
    push_copy_timestamp();
    sleep_ms(200);
    push_keydown(SDLK_d, SDL_SCANCODE_D, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
    sleep_ms(700);
    push_copy_timestamp();
    sleep_ms(200);
    push_keydown(SDLK_a, SDL_SCANCODE_A, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
    sleep_ms(700);
    push_copy_timestamp();
    sleep_ms(200);
  } else if (scenario == Scenario::KeyframeNavigationUnionTimeShift) {
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(300);
    const char* frame_shift = std::getenv("KEYFRAME_NAV_FRAME_SHIFT");
    if (frame_shift != nullptr && frame_shift[0] != '\0') {
      const int presses = std::max(1, std::atoi(frame_shift));
      for (int i = 0; i < presses; ++i) {
        push_keydown(SDLK_EQUALS, SDL_SCANCODE_EQUALS, 0, KMOD_CTRL);
        sleep_ms(500);
      }
      sleep_ms(400);
    }
    const char* warmup_env = std::getenv("KEYFRAME_NAV_WARMUP_D");
    const int warmup_d = (warmup_env != nullptr && warmup_env[0] != '\0') ? std::max(0, std::atoi(warmup_env)) : 0;
    for (int i = 0; i < warmup_d; ++i) {
      push_keydown(SDLK_d, SDL_SCANCODE_D, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
      sleep_ms(900);
    }
    sleep_ms(300);
    push_copy_timestamp();
    sleep_ms(200);
    const char* steps_env = std::getenv("KEYFRAME_NAV_STEPS");
    const int step_count = (steps_env != nullptr && steps_env[0] != '\0') ? std::max(1, std::atoi(steps_env)) : 3;
    const char* backward_env = std::getenv("KEYFRAME_NAV_BACKWARD");
    const bool backward = backward_env != nullptr && backward_env[0] == '1';
    const SDL_Keycode key = backward ? SDLK_a : SDLK_d;
    const SDL_Scancode scan = backward ? SDL_SCANCODE_A : SDL_SCANCODE_D;
    for (int i = 0; i < step_count; ++i) {
      push_keydown(key, scan, 0, static_cast<SDL_Keymod>(KMOD_CTRL | KMOD_SHIFT));
      sleep_ms(2000);
      push_copy_timestamp();
      sleep_ms(250);
    }
  } else if (scenario == Scenario::BufferForwardOnly || scenario == Scenario::BufferPingPong) {
    // Fill the 3-slot history, pause so offset 0 stays on the newest frame,
    // then slow to 1/16x so timer-driven steps are ~640 ms. J is the
    // ordinary playback-speed control (6 presses per octave).
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(300);
    for (int i = 0; i < 24; ++i) {
      push_keydown(SDLK_j, SDL_SCANCODE_J, 0);
    }
    sleep_ms(200);
    push_copy_timestamp();
    sleep_ms(200);
    if (scenario == Scenario::BufferForwardOnly) {
      push_keydown(SDLK_PERIOD, SDL_SCANCODE_PERIOD, 0);
    } else {
      push_keydown(SDLK_COMMA, SDL_SCANCODE_COMMA, 0);
    }
    // Period/comma pauses decode and starts timer-driven offset updates.
    // The first step is armed on that same iteration after refresh.
    // 1/16x makes each step ~640 ms; copy every 200 ms and collapse
    // consecutive duplicates so we recover the logical sequence.
    for (int i = 0; i < 18; ++i) {
      sleep_ms(200);
      push_copy_timestamp();
    }
    sleep_ms(200);
  } else if (scenario == Scenario::SeekBurstForward || scenario == Scenario::SeekBurstMixed) {
    // Pause so clipboard copies are a stable playhead and the post-storm
    // Shift+D health probe is a one-frame fetch, not continuous playback.
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(400);
    push_copy_timestamp();
    sleep_ms(300);

    if (scenario == Scenario::SeekBurstForward) {
      // Same-poll accumulation: five RIGHT with no delay.
      for (int i = 0; i < 5; ++i) {
        push_relative_seek(1);
      }
      // Faster-than-service burst. Drain/seek is tens of ms; 10 ms spacing
      // lets later events queue while the first transaction is still running.
      for (int i = 0; i < 5; ++i) {
        push_relative_seek(1);
        sleep_ms(10);
      }
      sleep_ms(1200);
    } else {
      // Move well into the 20 s clip before LEFT events so we stay off 0.
      for (int i = 0; i < 5; ++i) {
        push_relative_seek(1);
      }
      sleep_ms(800);
      const int mixed[] = {1, 1, 1, 1, 1, -1, -1, -1, 1, 1, 1, 1, -1, -1};
      for (const int direction : mixed) {
        push_relative_seek(direction);
        sleep_ms(10);
      }
      sleep_ms(2000);
    }

    push_copy_timestamp();
    sleep_ms(300);
    push_keydown(SDLK_d, SDL_SCANCODE_D, 0, KMOD_SHIFT);
    sleep_ms(500);
    push_copy_timestamp();
    sleep_ms(200);
  } else if (scenario == Scenario::CropCopy) {
    // Window and generated clips are both 320x180. Distinct drags produce
    // distinct crop= tokens in the Shift+X filter dump. Do not hard-code the
    // mapped pixels; later assertions compare those captured tokens.
    const int left_x0 = 20, left_y0 = 20, left_x1 = 140, left_y1 = 100;
    const int right1_x0 = 50, right1_y0 = 40, right1_x1 = 170, right1_y1 = 120;
    const int right0_x0 = 30, right0_y0 = 50, right0_x1 = 150, right0_y1 = 130;
    const int right1_again_x0 = 70, right1_again_y0 = 20, right1_again_x1 = 190, right1_again_y1 = 90;

    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(400);

    // 1. Normal Ctrl+Shift+L: crop logical left, give R0 and R1 different crops,
    //    leave R2 uncropped, copy visual left to all rights, then undo.
    interactive_crop(SDLK_l, SDL_SCANCODE_L, left_x0, left_y0, left_x1, left_y1);
    dump_display_filters();
    toggle_swap();
    dump_display_filters();
    toggle_swap();

    select_right_video(1);
    interactive_crop(SDLK_r, SDL_SCANCODE_R, right1_x0, right1_y0, right1_x1, right1_y1);
    dump_display_filters();

    select_right_video(0);
    interactive_crop(SDLK_r, SDL_SCANCODE_R, right0_x0, right0_y0, right0_x1, right0_y1);
    dump_display_filters();

    copy_visual_left_crop_to_others();
    dump_display_filters();
    select_right_video(1);
    dump_display_filters();
    select_right_video(2);
    dump_display_filters();

    undo_last_crop();
    select_right_video(0);
    dump_display_filters();
    select_right_video(1);
    dump_display_filters();
    select_right_video(2);
    dump_display_filters();

    // 2. Swapped Ctrl+Shift+L: R1 on visual left is the source. Only the visual
    //    right (logical left) is a destination.
    select_right_video(1);
    toggle_swap();
    copy_visual_left_crop_to_others();
    dump_display_filters();
    select_right_video(0);
    dump_display_filters();
    select_right_video(2);
    dump_display_filters();

    undo_last_crop();
    select_right_video(0);
    dump_display_filters();
    select_right_video(1);
    dump_display_filters();
    select_right_video(2);
    dump_display_filters();
    toggle_swap();

    // 3. Ctrl+Shift+R normal (current right -> logical left) then swapped
    //    (logical left -> current right).
    select_right_video(1);
    copy_visual_right_crop_to_left();
    dump_display_filters();
    select_right_video(0);
    dump_display_filters();
    undo_last_crop();
    select_right_video(1);
    dump_display_filters();

    select_right_video(0);
    toggle_swap();
    copy_visual_right_crop_to_left();
    dump_display_filters();
    undo_last_crop();
    dump_display_filters();
    toggle_swap();

    // 4. Undo keeps the concrete side from the operation, not the later
    //    selected right. Crop R1, switch to R2, Backspace.
    select_right_video(1);
    interactive_crop(SDLK_r, SDL_SCANCODE_R, right1_again_x0, right1_again_y0, right1_again_x1, right1_again_y1);
    dump_display_filters();
    select_right_video(2);
    dump_display_filters();
    undo_last_crop();
    dump_display_filters();
    select_right_video(1);
    dump_display_filters();

    // 5. Ctrl+L/R follow visual sides and only the displayed video.
    clear_visual_left_crop();
    dump_display_filters();
    undo_last_crop();
    clear_visual_right_crop();
    if (!wait_for_crop_copy_post_ctrl_r_clear_state()) {
      push_quit();
      finished.store(true);
      watchdog.join();
      return;
    }
    dump_display_filters();
    select_right_video(0);
    dump_display_filters();
    undo_last_crop();
    select_right_video(1);
    dump_display_filters();

    toggle_swap();
    clear_visual_left_crop();
    dump_display_filters();
    select_right_video(0);
    dump_display_filters();
    undo_last_crop();
    select_right_video(1);
    clear_visual_right_crop();
    dump_display_filters();
    undo_last_crop();
    dump_display_filters();
    toggle_swap();
  } else if (scenario == Scenario::InteractiveCrop) {
    // Left dest is 160x90 after post-scale; right dest stays 320x180. Canvas
    // is 320x180. The same window drag must become crop=120:80:20:20 in
    // 320x180 crop-space, not dest-space crop=60:40:10:10.
    const int x0 = 20, y0 = 20, x1 = 140, y1 = 100;

    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(400);

    dump_display_filters();
    interactive_crop(SDLK_b, SDL_SCANCODE_B, x0, y0, x1, y1);
    dump_display_filters();
    interactive_crop(SDLK_l, SDL_SCANCODE_L, x0, y0, x1, y1);
    dump_display_filters();
    undo_last_crop();
    dump_display_filters();
    interactive_crop(SDLK_r, SDL_SCANCODE_R, x0, y0, x1, y1);
    dump_display_filters();
    toggle_swap();
    interactive_crop(SDLK_l, SDL_SCANCODE_L, x0, y0, x1, y1);
    dump_display_filters();
  } else if (scenario == Scenario::DynamicSwap) {
    // 320x240 (4:3) left, 640x360 (16:9) right. Canvas/window stay 16:9;
    // Dynamic pillarboxes to the original left. Swap must not retarget either.
    push_keydown(SDLK_SPACE, SDL_SCANCODE_SPACE, 0);
    sleep_ms(400);
    dump_display_filters();
    toggle_swap();
    dump_display_filters();
  }

  push_quit();
  finished.store(true);
  watchdog.join();
}

VideoCompareConfig make_config(const std::vector<std::string>& files) {
  VideoCompareConfig config;
  config.window_size = std::make_tuple(320, 180);
  config.left.file_name = files[0];

  for (size_t i = 1; i < files.size(); ++i) {
    InputVideo right;
    right.side = Side::Right(i - 1);
    right.side_description = "Right";
    right.file_name = files[i];
    config.right_videos.push_back(right);
  }

  return config;
}

int run_scenario(const Scenario scenario, const std::vector<std::string>& files) {
  std::vector<std::string> resolved = files;
  std::unique_ptr<DumpDirectory> dump_directory;
  if (scenario == Scenario::StillSeek) {
    for (std::string& file : resolved) {
      file = resolve_path(file);
    }
    dump_directory.reset(new DumpDirectory("vc-still-seek-"));
  }

  for (const std::string& file : resolved) {
    require_readable_file(file.c_str());
  }

  prepare_dummy_software_sdl();
  reset_helper_counters();

  VideoCompareConfig config = make_config(resolved);
  const char* shift_env = std::getenv("KEYFRAME_NAV_SHIFT_MS");
  if (shift_env != nullptr && shift_env[0] != '\0') {
    config.time_shift.offset_ms = std::strtol(shift_env, nullptr, 10);
  }
  if (scenario == Scenario::SyncMismatch || scenario == Scenario::MultiRightSync || scenario == Scenario::CropCopy || scenario == Scenario::InteractiveCrop) {
    // Auto fps= would otherwise lift 25 fps to 30 and hide the sync path.
    // crop-copy also wants a clean Shift+X chain so crop= is the only extra filter.
    config.disable_auto_filters = true;
  }
  if (scenario == Scenario::InteractiveCrop) {
    config.left.video_filters = "scale=160:90";
    config.right_videos[0].video_filters = "hflip";
  }
  if (scenario == Scenario::DynamicSwap) {
    // Keep pre-canvas sizes at 320x240 and 640x360. Dynamic letterboxes from the left DAR.
    config.disable_auto_filters = true;
    config.aspect_view_mode = Display::AspectViewMode::Dynamic;
  }
  if (scenario == Scenario::BufferForwardOnly || scenario == Scenario::BufferPingPong) {
    // 3 slots: newest / middle / oldest. Smallest history that still has
    // an interior frame plus both endpoints for wrap and bounce.
    config.frame_buffer_size = 3;
  }

  IntegrationStdoutCapture integration_stdout;
  std::vector<size_t> official_display_dump_indices;
  g_crop_copy_checkpoint.failed.store(false);
  {
    std::lock_guard<std::mutex> lock(g_crop_copy_checkpoint.message_mutex);
    g_crop_copy_checkpoint.message.clear();
  }
  if (scenario == Scenario::CropCopy) {
    g_integration_stdout_capture = &integration_stdout;
    g_official_display_dump_indices = &official_display_dump_indices;
  }
  std::streambuf* const old_out = std::cout.rdbuf(&integration_stdout.stream_buf);

  std::atomic<bool> finished{false};
  std::thread helper;
  int exit_code = EXIT_SUCCESS;
  std::vector<playback_keyframe::KeyframeNavigationLandingObservation> keyframe_landing_observations;
  playback_keyframe::KeyframeNavigationLandingObserver keyframe_landing_observer;
  if (scenario == Scenario::KeyframeNavigationUnionTimeShift) {
    keyframe_landing_observer = [&keyframe_landing_observations](const playback_keyframe::KeyframeNavigationLandingObservation& observation) { keyframe_landing_observations.push_back(observation); };
  }

  try {
    VideoCompare video_compare(config, keyframe_landing_observer);
    helper = std::thread(run_event_script, scenario, std::ref(finished));
    video_compare();
  } catch (const std::exception& exception) {
    finished.store(true);
    push_quit();
    std::cout.rdbuf(old_out);
    g_integration_stdout_capture = nullptr;
    g_official_display_dump_indices = nullptr;
    if (helper.joinable()) {
      helper.join();
    }
    std::fprintf(stderr, "FAIL VideoCompare threw: %s\n", exception.what());
    return EXIT_FAILURE;
  }

  std::cout.rdbuf(old_out);
  g_integration_stdout_capture = nullptr;
  g_official_display_dump_indices = nullptr;
  if (helper.joinable()) {
    helper.join();
  }

  const std::string output = integration_stdout.snapshot();
  std::cout << output;

  if (scenario == Scenario::EventInjection) {
    if (output.find("Active right video: 2/2") == std::string::npos) {
      std::fprintf(stderr, "FAIL injected Tab did not produce 'Active right video: 2/2'\n");
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else {
      std::printf("PASS injected Tab switched active right to 2/2\n");
    }
  } else if (scenario == Scenario::Seek) {
    const std::vector<double> times = copied_positions(output);
    if (times.size() < 5) {
      std::fprintf(stderr, "FAIL seek expected 5 copied timestamps, got %zu\n", times.size());
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else {
      const double forward = times[1] - times[0];
      const double backward = times[1] - times[2];
      const double eof_drift = std::fabs(times[4] - times[3]);
      std::printf("seek timestamps: before=%.3f after+1s=%.3f after-1s=%.3f pre_eof=%.3f after_eof_fail=%.3f\n", times[0], times[1], times[2], times[3], times[4]);
      // Intra mpeg4 should land near ±1s. Allow decoder/PTS slack without
      // accepting a no-op (identical timestamps).
      if (forward < 0.40 || forward > 1.75) {
        std::fprintf(stderr, "FAIL forward seek delta %.3f not in [0.40, 1.75]\n", forward);
        exit_code = EXIT_FAILURE;
      } else if (backward < 0.40 || backward > 1.75) {
        std::fprintf(stderr, "FAIL backward seek delta %.3f not in [0.40, 1.75]\n", backward);
        exit_code = EXIT_FAILURE;
      } else if (eof_drift > 0.25) {
        std::fprintf(stderr, "FAIL forward seek past EOF drift %.3f exceeds 0.25s (%.3f vs %.3f)\n", eof_drift, times[4], times[3]);
        exit_code = EXIT_FAILURE;
      } else if (times[3] < 2.5) {
        std::fprintf(stderr, "FAIL seek past EOF precondition: pre-EOF PTS %.3f too early\n", times[3]);
        exit_code = EXIT_FAILURE;
      } else {
        std::printf("PASS keyboard seek moved presented PTS forward then backward\n");
        std::printf("PASS ordinary forward seek past EOF kept displayed PTS stable (drift=%.3f)\n", eof_drift);
      }
    }
  } else if (scenario == Scenario::StillSeek) {
    // A still seek often lands on the same PTS, so clipboard deltas are not
    // used as proof. F after the seek proves the post-seek stills were still
    // presentable (frames in the display buffer, refresh + PNG write succeeded).
    // It does not prove pixel-perfect equality or that PTS moved.
    const char* saved = "Saved screenshot_1_0001.png, screenshot_2_0001.png and screenshot_1_screenshot_2_osd_0001.png";
    const char* dumps[] = {"screenshot_1_0001.png", "screenshot_2_0001.png", "screenshot_1_screenshot_2_osd_0001.png"};
    if (output.find(saved) == std::string::npos) {
      std::fprintf(stderr, "FAIL still-seek did not save post-seek PNGs\n");
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else {
      bool all_present = true;
      for (const char* dump : dumps) {
        if (!file_is_non_empty(dump)) {
          std::fprintf(stderr, "FAIL still-seek dump missing or empty: %s\n", dump);
          all_present = false;
        }
      }
      if (all_present) {
        std::printf("PASS still-seek left presentable frames after skip_update (PNG dumps exist and are non-empty)\n");
      } else {
        exit_code = EXIT_FAILURE;
      }
    }
  } else if (scenario == Scenario::SyncMismatch) {
    // Clipboard is left PTS only. A mid-clip timestamp after unpaused 25/30
    // playback shows the loop kept presenting; it does not prove both sides'
    // PTS stayed inside min_delta. Filter strings go to av_log/stderr, not here.
    const std::vector<double> times = copied_positions(output);
    if (times.size() < 1) {
      std::fprintf(stderr, "FAIL sync-mismatch expected a copied timestamp after playback\n");
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else if (times[0] < 0.40 || times[0] > 2.20) {
      std::fprintf(stderr, "FAIL sync-mismatch copied PTS %.3f not in [0.40, 2.20]\n", times[0]);
      exit_code = EXIT_FAILURE;
    } else {
      std::printf("PASS sync-mismatch presented left PTS %.3f after mismatched-rate playback\n", times[0]);
    }
  } else if (scenario == Scenario::MultiRightSync) {
    // Clipboard is left PTS only; Tab proves the second right is still live.
    // Same-iteration double left-pop is inferred from the frozen-pts sync loop
    // plus coverage, not asserted from this output.
    const std::vector<double> times = copied_positions(output);
    if (times.size() < 1) {
      std::fprintf(stderr, "FAIL multi-right-sync expected a copied timestamp after playback\n");
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else if (times[0] < 0.40 || times[0] > 3.20) {
      std::fprintf(stderr, "FAIL multi-right-sync copied PTS %.3f not in [0.40, 3.20]\n", times[0]);
      exit_code = EXIT_FAILURE;
    } else if (output.find("Active right video: 2/2") == std::string::npos) {
      std::fprintf(stderr, "FAIL multi-right-sync Tab did not produce 'Active right video: 2/2'\n");
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else {
      std::printf("PASS multi-right-sync presented left PTS %.3f and switched active right to 2/2\n", times[0]);
    }
  } else if (scenario == Scenario::FrameNavigation) {
    const std::vector<double> times = copied_positions(output);
    if (times.size() < 3) {
      std::fprintf(stderr, "FAIL frame-navigation expected 3 copied timestamps, got %zu\n", times.size());
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else {
      const double t0 = times[0];
      const double t1 = times[1];
      const double t2 = times[2];
      const double forward = t1 - t0;
      const double backward = t2 - t1;
      const double roundtrip = t2 - t0;
      std::printf("frame-navigation timestamps: T0=%.3f T1=%.3f T2=%.3f forward=%.3f backward=%.3f roundtrip=%.3f\n", t0, t1, t2, forward, backward, roundtrip);
      // 25 fps intra clips should move exactly one frame (~0.040 s).
      // Millisecond clipboard formatting can land on 0.039/0.041.
      if (!(t1 > t0)) {
        std::fprintf(stderr, "FAIL T1 (%.3f) is not greater than T0 (%.3f); forward=%.3f backward=%.3f\n", t1, t0, forward, backward);
        exit_code = EXIT_FAILURE;
      } else if (forward < 0.030 || forward > 0.050) {
        std::fprintf(stderr, "FAIL forward delta %.3f not in [0.030, 0.050]; T0=%.3f T1=%.3f T2=%.3f backward=%.3f\n", forward, t0, t1, t2, backward);
        exit_code = EXIT_FAILURE;
      } else if (!(t2 < t1)) {
        std::fprintf(stderr, "FAIL T2 (%.3f) is not less than T1 (%.3f); T0=%.3f forward=%.3f backward=%.3f\n", t2, t1, t0, forward, backward);
        exit_code = EXIT_FAILURE;
      } else if (backward < -0.050 || backward > -0.030) {
        std::fprintf(stderr, "FAIL backward delta %.3f not in [-0.050, -0.030]; T0=%.3f T1=%.3f T2=%.3f forward=%.3f\n", backward, t0, t1, t2, forward);
        exit_code = EXIT_FAILURE;
      } else if (std::fabs(roundtrip) > 0.015) {
        std::fprintf(stderr, "FAIL |T2-T0| %.3f exceeds 0.015; T0=%.3f T1=%.3f T2=%.3f forward=%.3f backward=%.3f\n", std::fabs(roundtrip), t0, t1, t2, forward, backward);
        exit_code = EXIT_FAILURE;
      } else {
        std::printf("PASS Shift+D / Shift+A presented one frame forward then one frame backward\n");
      }
    }
  } else if (scenario == Scenario::KeyframeNavigation) {
    const std::vector<double> times = copied_positions(output);
    if (times.size() < 7) {
      std::fprintf(stderr, "FAIL keyframe-navigation expected 7 copied timestamps, got %zu\n", times.size());
      exit_code = EXIT_FAILURE;
    } else if (output.find("Keyframe navigation unavailable") != std::string::npos) {
      std::fprintf(stderr, "FAIL keyframe-navigation reported unavailable on indexed fixtures\n");
      exit_code = EXIT_FAILURE;
    } else {
      const double t0 = times[0];
      const double after_next_kf = times[1];
      const double after_next_boundary = times[2];
      const double after_prev_kf = times[3];
      const double after_prev_boundary = times[4];
      const double to_keyframe = after_next_kf - t0;
      const double return_delta = after_prev_kf - after_next_kf;
      std::printf("keyframe-navigation timestamps: T0=%.3f next_kf=%.3f prev_kf=%.3f jump=%.3f return=%.3f\n", t0, after_next_kf, after_prev_kf, to_keyframe, return_delta);
      if (to_keyframe < 1.90 || to_keyframe > 2.10) {
        std::fprintf(stderr, "FAIL next-keyframe jump %.3f not in [1.90, 2.10] (50-frame GOP at 25 fps)\n", to_keyframe);
        exit_code = EXIT_FAILURE;
      } else if (return_delta > -1.90 || return_delta < -2.10) {
        std::fprintf(stderr, "FAIL prev-keyframe delta %.3f not in [-2.10, -1.90]\n", return_delta);
        exit_code = EXIT_FAILURE;
      } else if (std::fabs(after_next_boundary - after_next_kf) > 0.05) {
        std::fprintf(stderr, "FAIL next-keyframe boundary changed position %.3f -> %.3f\n", after_next_kf, after_next_boundary);
        exit_code = EXIT_FAILURE;
      } else if (std::fabs(after_prev_kf) > 0.05) {
        std::fprintf(stderr, "FAIL prev-keyframe did not land on first keyframe (got %.3f)\n", after_prev_kf);
        exit_code = EXIT_FAILURE;
      } else if (std::fabs(after_prev_boundary - after_prev_kf) > 0.05) {
        std::fprintf(stderr, "FAIL prev-keyframe boundary changed position %.3f -> %.3f\n", after_prev_kf, after_prev_boundary);
        exit_code = EXIT_FAILURE;
      } else if (times[5] < after_next_kf + 0.50 || times[5] > after_next_kf + 1.70) {
        std::fprintf(stderr, "FAIL final-GOP setup did not advance into final GOP: %.3f -> %.3f\n", after_next_kf, times[5]);
        exit_code = EXIT_FAILURE;
      } else if (std::fabs(times[6] - times[5]) > 0.05) {
        std::fprintf(stderr, "FAIL next-keyframe from final GOP rewound: %.3f -> %.3f\n", times[5], times[6]);
        exit_code = EXIT_FAILURE;
      } else {
        std::printf("PASS Ctrl+Shift+D/A seeked coherent keyframes on left or active right input\n");
      }
    }
  } else if (scenario == Scenario::KeyframeNavigationGop25) {
    const std::vector<double> times = copied_positions(output);
    if (times.size() < 4) {
      std::fprintf(stderr, "FAIL keyframe-navigation-gop25 expected 4 left copied timestamps, got %zu\n", times.size());
      exit_code = EXIT_FAILURE;
    } else if (output.find("Keyframe navigation unavailable") != std::string::npos) {
      std::fprintf(stderr, "FAIL keyframe-navigation-gop25 reported unavailable on indexed fixtures\n");
      exit_code = EXIT_FAILURE;
    } else {
      const double t0 = times[0];
      const double d1 = times[1] - t0;
      const double d2 = times[2] - times[1];
      const double d_back = times[3] - times[2];
      std::printf("keyframe-navigation-gop25 symmetric fixtures: T0=%.3f T1=%.3f T2=%.3f T3=%.3f d1=%.3f d2=%.3f back=%.3f\n", t0, times[1], times[2], times[3], d1, d2, d_back);

      std::vector<double> expect_forward_steps;
      std::vector<double> expect_backward_steps;
      if (playback_keyframe::libavformat_index_api_available() && !resolved.empty()) {
        AVFormatContext* format_context = nullptr;
        if (avformat_open_input(&format_context, resolved[0].c_str(), nullptr, nullptr) >= 0 && avformat_find_stream_info(format_context, nullptr) >= 0) {
          const int video_stream = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
          const AVStream* stream = format_context->streams[video_stream];
          const float start_time = stream->start_time != AV_NOPTS_VALUE ? static_cast<float>(av_q2d(stream->time_base) * stream->start_time) : 0.0F;
          const playback_keyframe::KeyframeIndexSnapshot snapshot = playback_keyframe::capture_verified_keyframe_index_snapshot(format_context, video_stream, start_time);
          std::vector<double> key_positions;
          for (const playback_keyframe::IndexEntry& entry : snapshot.entries) {
            if ((entry.flags & AVINDEX_KEYFRAME) == 0) {
              continue;
            }
            const int64_t pts = playback_keyframe::index_entry_presentation_timestamp(snapshot, entry);
            if (pts == AV_NOPTS_VALUE) {
              continue;
            }
            key_positions.push_back(playback_keyframe::stream_timestamp_to_demuxer_seconds(pts, snapshot.time_base, snapshot.video_start_time_seconds));
          }
          if (key_positions.size() >= 2) {
            const auto adjacent_forward_from = [&](const double reference) {
              const playback_keyframe::AdjacentKeyframe next = playback_keyframe::find_adjacent_keyframe(snapshot, +1, static_cast<float>(reference));
              if (next.status == playback_keyframe::AdjacentKeyframeStatus::Ok) {
                return static_cast<double>(next.demuxer_position_seconds) - reference;
              }
              return 0.0;
            };
            expect_forward_steps.push_back(adjacent_forward_from(t0));
            expect_forward_steps.push_back(adjacent_forward_from(times[1]));
            const playback_keyframe::AdjacentKeyframe back = playback_keyframe::find_adjacent_keyframe(snapshot, -1, static_cast<float>(times[2]));
            if (back.status == playback_keyframe::AdjacentKeyframeStatus::Ok) {
              expect_backward_steps.push_back(static_cast<double>(back.demuxer_position_seconds) - times[2]);
            }
          }
          avformat_close_input(&format_context);
        }
      }

      constexpr double kTol = 0.08;
      const auto step_near = [&](const double got, const double expect) { return std::fabs(got - expect) <= kTol; };
      const double exp_d1 = expect_forward_steps.size() > 0 ? expect_forward_steps[0] : 1.0;
      const double exp_d2 = expect_forward_steps.size() > 1 ? expect_forward_steps[1] : exp_d1;
      const double exp_back = expect_backward_steps.size() > 0 ? expect_backward_steps[0] : -exp_d2;
      if (!step_near(d1, exp_d1)) {
        std::fprintf(stderr, "FAIL GOP25 forward step 1 delta %.3f not ~%.3f\n", d1, exp_d1);
        exit_code = EXIT_FAILURE;
      } else if (!step_near(d2, exp_d2)) {
        std::fprintf(stderr, "FAIL GOP25 forward step 2 delta %.3f not ~%.3f\n", d2, exp_d2);
        exit_code = EXIT_FAILURE;
      } else if (!step_near(d_back, exp_back)) {
        std::fprintf(stderr, "FAIL GOP25 backward step delta %.3f not ~%.3f\n", d_back, exp_back);
        exit_code = EXIT_FAILURE;
      } else {
        std::printf("PASS Ctrl+Shift+D/A keyframe steps on symmetric GOP-25 fixtures (left PTS)\n");
      }
    }
  } else if (scenario == Scenario::KeyframeNavigationAsymmetric) {
    const std::vector<double> times = copied_positions(output);
    if (times.size() < 4) {
      std::fprintf(stderr, "FAIL keyframe-navigation-asymmetric expected 4 left copied timestamps, got %zu\n", times.size());
      exit_code = EXIT_FAILURE;
    } else if (output.find("Keyframe navigation unavailable") != std::string::npos) {
      std::fprintf(stderr, "FAIL keyframe-navigation-asymmetric reported unavailable on indexed fixtures\n");
      exit_code = EXIT_FAILURE;
    } else {
      const double t0 = times[0];
      const double d1 = times[1] - t0;
      const double d2 = times[2] - times[1];
      const double d_back = times[3] - times[2];
      std::printf("keyframe-navigation-asymmetric: T0=%.3f T1=%.3f T2=%.3f T3=%.3f d1=%.3f d2=%.3f back=%.3f\n", t0, times[1], times[2], times[3], d1, d2, d_back);

      std::vector<double> union_steps_forward;
      std::vector<double> union_steps_backward;
      if (playback_keyframe::libavformat_index_api_available() && resolved.size() >= 2) {
        auto key_positions_for = [](const std::string& path) {
          std::vector<double> positions;
          AVFormatContext* format_context = nullptr;
          if (avformat_open_input(&format_context, path.c_str(), nullptr, nullptr) < 0) {
            return positions;
          }
          if (avformat_find_stream_info(format_context, nullptr) < 0) {
            avformat_close_input(&format_context);
            return positions;
          }
          const int video_stream = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
          const AVStream* stream = format_context->streams[video_stream];
          const float start_time = stream->start_time != AV_NOPTS_VALUE ? static_cast<float>(av_q2d(stream->time_base) * stream->start_time) : 0.0F;
          const playback_keyframe::KeyframeIndexSnapshot snapshot = playback_keyframe::capture_verified_keyframe_index_snapshot(format_context, video_stream, start_time);
          for (const playback_keyframe::IndexEntry& entry : snapshot.entries) {
            if ((entry.flags & AVINDEX_KEYFRAME) == 0) {
              continue;
            }
            const int64_t pts = playback_keyframe::index_entry_presentation_timestamp(snapshot, entry);
            if (pts == AV_NOPTS_VALUE) {
              continue;
            }
            positions.push_back(playback_keyframe::stream_timestamp_to_demuxer_seconds(pts, snapshot.time_base, snapshot.video_start_time_seconds));
          }
          avformat_close_input(&format_context);
          std::sort(positions.begin(), positions.end());
          positions.erase(std::unique(positions.begin(), positions.end(), [](const double a, const double b) { return std::fabs(a - b) <= 0.04; }), positions.end());
          return positions;
        };

        const std::vector<double> left_keys = key_positions_for(resolved[0]);
        const std::vector<double> right_keys = key_positions_for(resolved[1]);
        std::vector<double> union_keys = left_keys;
        for (const double key : right_keys) {
          const bool duplicate = std::any_of(union_keys.begin(), union_keys.end(), [&](const double existing) { return std::fabs(existing - key) <= 0.04; });
          if (!duplicate) {
            union_keys.push_back(key);
          }
        }
        std::sort(union_keys.begin(), union_keys.end());

        const auto forward_delta_from = [&](const double reference) {
          for (const double key : union_keys) {
            if (key > reference + 0.04) {
              return key - reference;
            }
          }
          return -1.0;
        };
        const auto backward_delta_from = [&](const double reference) {
          for (auto it = union_keys.rbegin(); it != union_keys.rend(); ++it) {
            if (*it < reference - 0.04) {
              return *it - reference;
            }
          }
          return 1.0;
        };

        union_steps_forward.push_back(forward_delta_from(t0));
        union_steps_forward.push_back(forward_delta_from(times[1]));
        union_steps_backward.push_back(backward_delta_from(times[2]));
      }

      const auto step_near = [](const double value, const double expected) { return std::fabs(value - expected) <= 0.12; };
      const double exp_d1 = union_steps_forward.size() > 0 ? union_steps_forward[0] : 1.0;
      const double exp_d2 = union_steps_forward.size() > 1 ? union_steps_forward[1] : 1.0;
      const double exp_back = union_steps_backward.empty() ? -1.0 : union_steps_backward[0];

      bool steps_ok = true;
      const auto expect_forward = [&](const double delta, const double expected, const char* label) {
        if (expected < 0.0) {
          if (std::fabs(delta) > 0.05) {
            std::fprintf(stderr, "FAIL %s delta %.3f expected boundary hold\n", label, delta);
            steps_ok = false;
          }
          return;
        }
        if (!step_near(delta, expected)) {
          std::fprintf(stderr, "FAIL %s delta %.3f not ~%.3f\n", label, delta, expected);
          steps_ok = false;
        }
      };

      expect_forward(d1, exp_d1, "asymmetric union forward step 1");
      expect_forward(d2, exp_d2, "asymmetric union forward step 2");
      if (!step_near(d_back, exp_back)) {
        std::fprintf(stderr, "FAIL asymmetric union backward step delta %.3f not ~%.3f\n", d_back, exp_back);
        steps_ok = false;
      }
      if (steps_ok) {
        std::printf("PASS Ctrl+Shift+D/A union keyframes on asymmetric GOP fixtures (order-independent)\n");
      } else {
        exit_code = EXIT_FAILURE;
      }
    }
  } else if (scenario == Scenario::KeyframeNavigationUnionTimeShift) {
    const auto near = [&](const double a, const double b) { return std::fabs(a - b) <= kUnionTol; };

    const char* steps_env = std::getenv("KEYFRAME_NAV_STEPS");
    const int step_count = (steps_env != nullptr && steps_env[0] != '\0') ? std::max(1, std::atoi(steps_env)) : 3;
    const std::vector<double> clipboard = copied_positions(output);
    std::vector<double> times;
    if (!clipboard.empty()) {
      // Only the paused baseline is read from the displayed left PTS.
      times.push_back(clipboard[0]);
    }
    if (clipboard.empty()) {
      std::fprintf(stderr, "FAIL union-time-shift missing baseline clipboard timestamp\n");
      exit_code = EXIT_FAILURE;
    } else if (keyframe_landing_observations.size() != static_cast<size_t>(step_count)) {
      std::fprintf(stderr, "FAIL union-time-shift expected %d landing observations, got %zu\n", step_count, keyframe_landing_observations.size());
      exit_code = EXIT_FAILURE;
    } else if (!append_verified_union_landings(times, keyframe_landing_observations)) {
      exit_code = EXIT_FAILURE;
    } else if (output.find("Keyframe navigation unavailable") != std::string::npos || output.find("LandingFailed") != std::string::npos || output.find("Landing failed") != std::string::npos) {
      std::fprintf(stderr, "FAIL union-time-shift reported keyframe navigation failure\n");
      exit_code = EXIT_FAILURE;
    } else {
      print_pts_stdout("union-time-shift observed: ", times);

      const char* expect_env = std::getenv("KEYFRAME_NAV_EXPECT");
      std::vector<double> expected;
      if (expect_env != nullptr && expect_env[0] != '\0') {
        const char* cursor = expect_env;
        while (*cursor != '\0') {
          char* end = nullptr;
          const double value = std::strtod(cursor, &end);
          if (end == cursor) {
            break;
          }
          expected.push_back(value);
          cursor = end;
          while (*cursor == ',' || *cursor == ' ') {
            ++cursor;
          }
        }
      }

      bool ok = true;
      if (!expected.empty()) {
        if (times.size() < expected.size()) {
          std::fprintf(stderr, "FAIL union-time-shift expected %zu timestamps, got %zu\n", expected.size(), times.size());
          ok = false;
        } else {
          for (size_t i = 0; i < expected.size(); ++i) {
            if (!near(times[i], expected[i])) {
              std::fprintf(stderr, "FAIL union-time-shift index %zu expected ~%.3f got %.3f (shift_ms=%" PRId64 ")\n", i, expected[i], times[i], config.time_shift.offset_ms);
              ok = false;
              break;
            }
            if (i > 0 && !(times[i] > times[i - 1] + 0.005)) {
              std::fprintf(stderr, "FAIL union-time-shift stall at %zu: %.3f -> %.3f (shift_ms=%" PRId64 ")\n", i, times[i - 1], times[i], config.time_shift.offset_ms);
              ok = false;
              break;
            }
          }
        }
      } else {
        const double shift_s = static_cast<double>(config.time_shift.offset_ms) / 1000.0;
        const bool backward = std::getenv("KEYFRAME_NAV_BACKWARD") != nullptr && std::getenv("KEYFRAME_NAV_BACKWARD")[0] == '1';
        const char* steps_env = std::getenv("KEYFRAME_NAV_STEPS");
        const int step_count = (steps_env != nullptr && steps_env[0] != '\0') ? std::max(1, std::atoi(steps_env)) : 3;
        const size_t expected_samples = static_cast<size_t>(step_count + 1);

        bool dense_left_fixture = false;
        bool sparse_left_reversed = false;
        if (resolved.size() >= 2) {
          dense_left_fixture = path_contains_basename(resolved[0], "kf_union_left_48") && path_contains_basename(resolved[1], "kf_union_right_sparse_48");
          sparse_left_reversed = path_contains_basename(resolved[0], "kf_union_right_sparse_48") && path_contains_basename(resolved[1], "kf_union_left_48");
        }

        if (backward) {
          for (size_t i = 1; i < times.size(); ++i) {
            const double delta = times[i] - times[i - 1];
            if (!(delta < -kUnionMinForwardStepS)) {
              std::fprintf(stderr, "FAIL union-time-shift backward stall at %zu: %.3f -> %.3f\n", i, times[i - 1], times[i]);
              ok = false;
              break;
            }
          }
        } else if ((dense_left_fixture || sparse_left_reversed) && std::fabs(shift_s - 0.04) <= 0.001) {
          const std::vector<double> lattice = dense_left_fixture ? synthetic_union_lattice_dense_left_sparse_right(shift_s) : synthetic_union_lattice_sparse_left_dense_right(shift_s);

          if (times.size() != expected_samples) {
            std::fprintf(stderr, "FAIL union-time-shift expected %zu timestamps (steps=%d), got %zu\n", expected_samples, step_count, times.size());
            ok = false;
          }
          if (ok && times[0] < 0.0) {
            std::fprintf(stderr, "FAIL union-time-shift initial timestamp %.3f invalid\n", times[0]);
            ok = false;
          }

          for (size_t i = 1; ok && i < times.size(); ++i) {
            const double delta = times[i] - times[i - 1];
            if (!(times[i] > times[i - 1] + kUnionMinForwardStepS)) {
              std::fprintf(stderr, "FAIL union-time-shift stall at %zu: %.3f -> %.3f (shift_ms=%" PRId64 ")\n", i, times[i - 1], times[i], config.time_shift.offset_ms);
              ok = false;
              break;
            }
            if (delta > kUnionMaxForwardStepS) {
              std::fprintf(stderr, "FAIL union-time-shift step %zu delta %.3f exceeds dense-GOP bound %.3f\n", i, delta, kUnionMaxForwardStepS);
              ok = false;
              break;
            }
            double successor = 0.0;
            if (!union_lattice_forward_successor(lattice, times[i - 1], &successor)) {
              std::fprintf(stderr, "FAIL union-time-shift no forward union key after %.3f at step %zu\n", times[i - 1], i);
              ok = false;
              break;
            }
            if (!near_union_lattice(times[i], successor)) {
              std::fprintf(stderr, "FAIL union-time-shift index %zu expected next union key ~%.3f got %.3f (prev=%.3f shift_ms=%" PRId64 ")\n", i, successor, times[i], times[i - 1], config.time_shift.offset_ms);
              ok = false;
              break;
            }
          }
        } else if (shift_s > 0.0 && !backward) {
          if (times.size() != expected_samples) {
            std::fprintf(stderr, "FAIL union-time-shift expected %zu timestamps (steps=%d), got %zu\n", expected_samples, step_count, times.size());
            ok = false;
          }
          for (size_t i = 1; ok && i < times.size(); ++i) {
            const double delta = times[i] - times[i - 1];
            if (!(delta > kUnionMinForwardStepS)) {
              std::fprintf(stderr, "FAIL union-time-shift forward stall at %zu: %.3f -> %.3f\n", i, times[i - 1], times[i]);
              ok = false;
              break;
            }
            if (delta > kUnionMaxForwardStepS) {
              std::fprintf(stderr, "FAIL union-time-shift step %zu delta %.3f exceeds plausible GOP step\n", i, delta);
              ok = false;
              break;
            }
          }
        }
      }

      if (ok) {
        std::printf("PASS union keyframe navigation with right time shift offset_ms=%" PRId64 "\n", config.time_shift.offset_ms);
      } else {
        exit_code = EXIT_FAILURE;
      }
    }
  } else if (scenario == Scenario::BufferForwardOnly || scenario == Scenario::BufferPingPong) {
    const std::vector<double> times = copied_positions(output);
    const char* name = (scenario == Scenario::BufferForwardOnly) ? "ForwardOnly" : "PingPong";
    print_pts_stdout((std::string(name) + " observed: ").c_str(), times);
    if (times.size() < 8) {
      std::fprintf(stderr, "FAIL %s expected at least 8 copied timestamps, got %zu\n", name, times.size());
      print_pts_sequence((std::string(name) + " observed: ").c_str(), times);
      exit_code = EXIT_FAILURE;
    } else {
      const double newest = times[0];
      const double middle = newest - 0.040;
      const double oldest = newest - 0.080;
      const std::vector<double> unique = unique_pts(std::vector<double>(times.begin() + 1, times.end()));
      print_pts_stdout((std::string(name) + " unique after start: ").c_str(), unique);

      bool lattice_ok = true;
      for (const double t : times) {
        if (!pts_near(t, newest) && !pts_near(t, middle) && !pts_near(t, oldest)) {
          lattice_ok = false;
        }
      }

      size_t start = 0;
      if (!unique.empty() && pts_near(unique[0], newest)) {
        start = 1;
      }

      if (!lattice_ok) {
        std::fprintf(stderr, "FAIL %s PTS left the 3-frame history lattice; newest=%.3f\n", name, newest);
        print_pts_sequence((std::string(name) + " observed: ").c_str(), times);
        exit_code = EXIT_FAILURE;
      } else if (unique.size() <= start + 2) {
        std::fprintf(stderr, "FAIL %s did not traverse history; newest=%.3f unique=%zu\n", name, newest, unique.size());
        print_pts_sequence((std::string(name) + " observed: ").c_str(), times);
        exit_code = EXIT_FAILURE;
      } else if (scenario == Scenario::BufferForwardOnly) {
        // Offset 0 wraps to last (oldest), then decrements toward newest.
        const double cycle[3] = {oldest, middle, newest};
        bool followed = true;
        for (size_t i = start, step = 0; i < unique.size(); ++i, ++step) {
          if (!pts_near(unique[i], cycle[step % 3])) {
            followed = false;
            break;
          }
        }
        if (!followed) {
          std::fprintf(stderr, "FAIL ForwardOnly unique sequence did not wrap oldest→middle→newest; newest=%.3f\n", newest);
          print_pts_sequence("ForwardOnly observed: ", times);
          exit_code = EXIT_FAILURE;
        } else {
          std::printf("PASS ForwardOnly wrapped from newest to oldest and replayed history\n");
        }
      } else {
        // From offset 0 / forward: flip, step to middle, continue to oldest,
        // bounce back through middle to newest. No stay-in-place endpoint.
        const double cycle[4] = {middle, oldest, middle, newest};
        bool followed = true;
        bool bounced = false;
        for (size_t i = start, step = 0; i < unique.size(); ++i, ++step) {
          if (!pts_near(unique[i], cycle[step % 4])) {
            followed = false;
            break;
          }
          if (step >= 2) {
            bounced = true;
          }
        }
        if (!followed || !bounced) {
          std::fprintf(stderr, "FAIL PingPong unique sequence did not bounce middle→oldest→middle→newest; newest=%.3f\n", newest);
          print_pts_sequence("PingPong observed: ", times);
          exit_code = EXIT_FAILURE;
        } else {
          std::printf("PASS PingPong reached the oldest frame and reversed through history\n");
        }
      }
    }
  } else if (is_stress_scenario(scenario)) {
    const std::vector<double> times = copied_positions(output);
    const double expected_storm = (scenario == Scenario::SeekBurstForward) ? 10.0 : 9.0;
    if (watchdog_fired.load()) {
      print_stress_failure(scenario, "in-process watchdog fired before helper finished", times, expected_storm);
      exit_code = EXIT_FAILURE;
    } else if (times.size() < 3) {
      print_stress_failure(scenario, "expected T0 T1 T2 copied timestamps", times, expected_storm);
      exit_code = EXIT_FAILURE;
    } else {
      const double t0 = times[0];
      const double t1 = times[1];
      const double t2 = times[2];
      const double storm = t1 - t0;
      const double step = t2 - t1;
      std::printf("%s timestamps: T0=%.3f T1=%.3f T2=%.3f storm=%.3f step=%.3f events=%d\n", scenario_name(scenario), t0, t1, t2, storm, step, events_pushed.load());
      if (t0 < 0.0 || t0 > 8.0) {
        print_stress_failure(scenario, "T0 is not a plausible paused start PTS", times, expected_storm);
        exit_code = EXIT_FAILURE;
      } else if (t1 < 0.0 || t1 > 19.50) {
        print_stress_failure(scenario, "T1 is outside the 20s fixture", times, expected_storm);
        exit_code = EXIT_FAILURE;
      } else if (!pts_near(storm, expected_storm)) {
        print_stress_failure(scenario, "storm delta does not match the sum of all seek commands", times, expected_storm);
        exit_code = EXIT_FAILURE;
      } else if (!(t2 > t1)) {
        print_stress_failure(scenario, "post-storm Shift+D did not advance PTS", times, expected_storm);
        exit_code = EXIT_FAILURE;
      } else if (step < 0.030 || step > 0.050) {
        print_stress_failure(scenario, "post-storm Shift+D was not one 25 fps frame", times, expected_storm);
        exit_code = EXIT_FAILURE;
      } else {
        std::printf("PASS %s accounted for all seek commands and Shift+D advanced one frame\n", scenario_name(scenario));
      }
    }
  } else if (scenario == Scenario::CropCopy) {
    auto fail_crop = [&](const char* message) {
      std::fprintf(stderr, "FAIL crop-copy: %s\n", message);
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    };

    std::vector<FilterDump> dumps;
    constexpr size_t kExpectedDumps = 33;
    if (g_crop_copy_checkpoint.failed.load()) {
      std::string checkpoint_message;
      {
        std::lock_guard<std::mutex> lock(g_crop_copy_checkpoint.message_mutex);
        checkpoint_message = g_crop_copy_checkpoint.message;
      }
      fail_crop(checkpoint_message.empty() ? "undo+Ctrl+R checkpoint failed" : checkpoint_message.c_str());
    } else {
      const std::vector<FilterDump> all_dumps = parse_display_filters(output);
      if (official_display_dump_indices.size() != kExpectedDumps) {
        fail_crop("expected 33 official Shift+X dump markers");
      } else {
        dumps.reserve(kExpectedDumps);
        for (size_t i = 0; i < official_display_dump_indices.size(); ++i) {
          const size_t index = official_display_dump_indices[i];
          if (index == kInvalidDisplayDumpIndex) {
            std::fprintf(stderr, "FAIL crop-copy: official dump %zu did not receive a new Display state line after Shift+X\n", i);
            fail_crop("official Shift+X dump missing delayed response");
            break;
          }
          if (index >= all_dumps.size()) {
            std::fprintf(stderr, "FAIL crop-copy: official dump %zu index %zu out of range (%zu Display state lines)\n", i, index, all_dumps.size());
            fail_crop("official Shift+X dump index out of range");
            break;
          }
          if (i > 0 && official_display_dump_indices[i - 1] != kInvalidDisplayDumpIndex && index <= official_display_dump_indices[i - 1]) {
            std::fprintf(stderr, "FAIL crop-copy: official dump %zu index %zu is not after dump %zu index %zu\n", i, index, i - 1, official_display_dump_indices[i - 1]);
            fail_crop("official Shift+X dump indices are not strictly increasing");
            break;
          }
          dumps.push_back(all_dumps[index]);
        }
      }
    }
    if (!dumps.empty()) {
      print_filter_dumps(dumps);
    }

    // dump  0 left crop A (R0 selected, uncropped)
    // dump  1 after Swap only (same selection, logical filters unchanged)
    // dump  2 R1 crop B
    // dump  3 R0 crop C
    // dump  4 after Ctrl+Shift+L, R0
    // dump  5 after Ctrl+Shift+L, R1
    // dump  6 after Ctrl+Shift+L, R2
    // dump  7 after undo, R0
    // dump  8 after undo, R1
    // dump  9 after undo, R2
    // dump 10 swapped Ctrl+Shift+L, R1 (source); only visual right / logical left updated
    // dump 11 swapped Ctrl+Shift+L, R0 unchanged
    // dump 12 swapped Ctrl+Shift+L, R2 unchanged
    // dump 13 swapped undo, R0
    // dump 14 swapped undo, R1
    // dump 15 swapped undo, R2
    // dump 16 Ctrl+Shift+R normal, R1 (source)
    // dump 17 Ctrl+Shift+R normal, R0 (untouched)
    // dump 18 Ctrl+Shift+R undo, R1
    // dump 19 Ctrl+Shift+R swapped, R0 (dest)
    // dump 20 Ctrl+Shift+R swapped undo, R0
    // dump 21 R1 recrop D
    // dump 22 R2 after R1 recrop
    // dump 23 after undo while R2 selected
    // dump 24 R1 after that undo
    // dump 25 Ctrl+L, R1 (left cleared)
    // dump 26 Ctrl+R, R1 (R1 cleared)
    // dump 27 Ctrl+R, R0 unchanged
    // dump 28 undo Ctrl+R, R1 restored
    // dump 29 swapped Ctrl+L, R1 cleared; left unchanged
    // dump 30 swapped Ctrl+L, R0 unchanged
    // dump 31 swapped Ctrl+R, left cleared; R1 unchanged
    // dump 32 undo swapped Ctrl+R, left restored
    if (dumps.size() < kExpectedDumps) {
      fail_crop("expected 33 Shift+X filter dumps");
    } else if (output.find("Copied left crop to all right videos") == std::string::npos) {
      fail_crop("normal/swapped Ctrl+Shift+L did not notify");
    } else if (output.find("Copied right crop to left") == std::string::npos) {
      fail_crop("Ctrl+Shift+R did not notify");
    } else if (output.find("Cleared left crop") == std::string::npos || output.find("Cleared right crop") == std::string::npos) {
      fail_crop("Ctrl+L/R did not notify");
    } else if (output.find("Active right video: 2/3") == std::string::npos || output.find("Active right video: 3/3") == std::string::npos) {
      fail_crop("did not select Right 1 and Right 2");
    } else {
      const std::string crop_a = crop_token(dumps[0].left);
      const std::string crop_b = crop_token(dumps[2].right);
      const std::string crop_c = crop_token(dumps[3].right);
      const std::string crop_d = crop_token(dumps[21].right);
      const std::string crop_a_640 = mapped_crop_token(crop_a, 320, 180, 640, 360);
      const std::string crop_a_160 = mapped_crop_token(crop_a, 320, 180, 160, 90);
      const std::string crop_b_320 = mapped_crop_token(crop_b, 320, 180, 320, 180);
      const std::string crop_b_640 = mapped_crop_token(crop_b, 320, 180, 640, 360);
      const std::string crop_b_160 = mapped_crop_token(crop_b, 320, 180, 160, 90);

      if (crop_a.empty() || crop_b.empty() || crop_c.empty() || crop_d.empty()) {
        fail_crop("interactive crops did not appear in the effective filter chain");
      } else if (crop_a == crop_b || crop_a == crop_c || crop_b == crop_c || crop_d == crop_b) {
        fail_crop("interactive crops were not distinct");
      } else if (dumps[0].swapped != "false" || dumps[0].right_id != "1") {
        fail_crop("initial Shift+X did not report swapped=false right=1");
      } else if (dumps[1].swapped != "true" || dumps[1].right_id != "1") {
        fail_crop("Swap did not report swapped=true with the same right=1");
      } else if (dumps[0].visual_left.empty() || dumps[0].visual_right.empty() || dumps[0].visual_left == dumps[0].visual_right) {
        fail_crop("pre-Swap visual sizes were missing or identical");
      } else if (dumps[1].visual_left != dumps[0].visual_right || dumps[1].visual_right != dumps[0].visual_left) {
        fail_crop("Swap did not exchange visual_left/visual_right");
      } else if (dumps[1].left != dumps[0].left || dumps[1].right != dumps[0].right) {
        fail_crop("Swap exchanged or changed logical filters_left/right");
      } else if (dumps[2].swapped != "false" || dumps[2].right_id != "2") {
        fail_crop("selecting Right 1 did not report right=2");
      } else if (crop_token(dumps[0].right) != "" || crop_token(dumps[2].left) != crop_a) {
        fail_crop("left crop leaked onto R0, or left changed while cropping R1");
      } else if (crop_token(dumps[4].left) != crop_a || crop_token(dumps[4].right) != crop_a_640) {
        fail_crop("normal Ctrl+Shift+L did not map left crop onto 640x360 R0, or changed left");
      } else if (crop_token(dumps[5].left) != crop_a || crop_token(dumps[5].right) != crop_a) {
        fail_crop("normal Ctrl+Shift+L did not copy left crop onto same-size R1");
      } else if (crop_token(dumps[6].left) != crop_a || crop_token(dumps[6].right) != crop_a_160) {
        fail_crop("normal Ctrl+Shift+L did not map left crop onto 160x90 R2");
      } else if (crop_token(dumps[7].left) != crop_a || crop_token(dumps[7].right) != crop_c) {
        fail_crop("undo Ctrl+Shift+L did not restore R0's previous crop, or changed left");
      } else if (crop_token(dumps[8].left) != crop_a || crop_token(dumps[8].right) != crop_b) {
        fail_crop("undo Ctrl+Shift+L did not restore R1's previous crop");
      } else if (crop_token(dumps[9].left) != crop_a || crop_token(dumps[9].right) != "") {
        fail_crop("undo Ctrl+Shift+L did not restore R2 to uncropped");
      } else if (crop_token(dumps[10].left) != crop_b_320 || crop_token(dumps[10].right) != crop_b) {
        fail_crop("swapped Ctrl+Shift+L did not copy R1 crop onto visual right (logical left), or changed the R1 source");
      } else if (crop_token(dumps[11].left) != crop_b_320 || crop_token(dumps[11].right) != crop_c) {
        fail_crop("swapped Ctrl+Shift+L changed R0, or lost the visual-right copy");
      } else if (crop_token(dumps[12].left) != crop_b_320 || crop_token(dumps[12].right) != "") {
        fail_crop("swapped Ctrl+Shift+L changed R2, or lost the visual-right copy");
      } else if (crop_token(dumps[13].left) != crop_a || crop_token(dumps[13].right) != crop_c) {
        fail_crop("undo swapped Ctrl+Shift+L did not restore left, or changed R0");
      } else if (crop_token(dumps[14].left) != crop_a || crop_token(dumps[14].right) != crop_b) {
        fail_crop("undo swapped Ctrl+Shift+L changed the R1 source");
      } else if (crop_token(dumps[15].left) != crop_a || crop_token(dumps[15].right) != "") {
        fail_crop("undo swapped Ctrl+Shift+L changed R2");
      } else if (crop_token(dumps[16].left) != crop_b_320 || crop_token(dumps[16].right) != crop_b) {
        fail_crop("Ctrl+Shift+R normal did not copy R1 crop onto logical left");
      } else if (crop_token(dumps[17].left) != crop_b_320 || crop_token(dumps[17].right) != crop_c) {
        fail_crop("Ctrl+Shift+R normal changed R0, or left was not the only destination");
      } else if (crop_token(dumps[18].left) != crop_a || crop_token(dumps[18].right) != crop_b) {
        fail_crop("undo Ctrl+Shift+R normal did not restore only logical left");
      } else if (crop_token(dumps[19].left) != crop_a || crop_token(dumps[19].right) != crop_a_640) {
        fail_crop("Ctrl+Shift+R swapped did not map logical left onto 640x360 R0");
      } else if (crop_token(dumps[20].left) != crop_a || crop_token(dumps[20].right) != crop_c) {
        fail_crop("undo Ctrl+Shift+R swapped did not restore only R0");
      } else if (crop_token(dumps[21].left) != crop_a || crop_token(dumps[21].right) != crop_d) {
        fail_crop("R1 recrop did not change only R1");
      } else if (crop_token(dumps[22].left) != crop_a || crop_token(dumps[22].right) != "") {
        fail_crop("R1 recrop changed R2");
      } else if (crop_token(dumps[23].left) != crop_a || crop_token(dumps[23].right) != "") {
        fail_crop("undo after selecting R2 changed R2");
      } else if (crop_token(dumps[24].left) != crop_a || crop_token(dumps[24].right) != crop_b) {
        fail_crop("undo after selecting R2 did not restore R1");
      } else if (crop_token(dumps[25].left) != "" || crop_token(dumps[25].right) != crop_b) {
        fail_crop("Ctrl+L did not clear only logical left");
      } else if (crop_token(dumps[26].left) != crop_a || crop_token(dumps[26].right) != "") {
        fail_crop("Ctrl+R did not clear only R1, or undo did not restore left");
      } else if (crop_token(dumps[27].left) != crop_a || crop_token(dumps[27].right) != crop_c) {
        fail_crop("Ctrl+R changed R0");
      } else if (crop_token(dumps[28].left) != crop_a || crop_token(dumps[28].right) != crop_b) {
        fail_crop("undo Ctrl+R did not restore R1");
      } else if (crop_token(dumps[29].left) != crop_a || crop_token(dumps[29].right) != "") {
        fail_crop("swapped Ctrl+L did not clear only R1");
      } else if (crop_token(dumps[30].left) != crop_a || crop_token(dumps[30].right) != crop_c) {
        fail_crop("swapped Ctrl+L changed R0");
      } else if (crop_token(dumps[31].left) != "" || crop_token(dumps[31].right) != crop_b) {
        fail_crop("swapped Ctrl+R did not clear only logical left, or undo did not restore R1");
      } else if (crop_token(dumps[32].left) != crop_a || crop_token(dumps[32].right) != crop_b) {
        fail_crop("undo swapped Ctrl+R did not restore left");
      } else {
        std::printf("PASS crop-copy wired normalized Ctrl+Shift+L/R and Ctrl+L/R (A=%s A640=%s A160=%s B=%s C=%s D=%s)\n", crop_a.c_str(), crop_a_640.c_str(), crop_a_160.c_str(), crop_b.c_str(), crop_c.c_str(), crop_d.c_str());
      }
    }
  } else if (scenario == Scenario::InteractiveCrop) {
    const std::vector<FilterDump> dumps = parse_display_filters(output);
    print_filter_dumps(dumps);

    auto fail_crop = [&](const char* message) {
      std::fprintf(stderr, "FAIL interactive-crop: %s\n", message);
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    };

    // dump 0 initial
    // dump 1 after refused Shift+B
    // dump 2 after Shift+L
    // dump 3 after Backspace
    // dump 4 after refused Shift+R
    // dump 5 after Swap + refused Shift+L
    constexpr size_t kExpectedDumps = 6;
    if (dumps.size() < kExpectedDumps) {
      fail_crop("expected 6 Shift+X filter dumps");
    } else if (output.find("Cannot apply crop: post-filters change spatial coordinates") == std::string::npos) {
      fail_crop("Shift+B did not notify that post-filters change spatial coordinates");
    } else if (output.find("Cannot crop right: post-filters change spatial coordinates") == std::string::npos) {
      fail_crop("Shift+R did not notify using the visual right side");
    } else if (output.find("Cannot crop left: post-filters change spatial coordinates") == std::string::npos) {
      fail_crop("swapped Shift+L did not notify using the visual left side");
    } else {
      const std::string left0 = crop_token(dumps[0].left);
      const std::string right0 = crop_token(dumps[0].right);
      const std::string left_b = crop_token(dumps[1].left);
      const std::string right_b = crop_token(dumps[1].right);
      const std::string left_l = crop_token(dumps[2].left);
      const std::string right_l = crop_token(dumps[2].right);
      const std::string left_undo = crop_token(dumps[3].left);
      const std::string right_undo = crop_token(dumps[3].right);
      const std::string left_r = crop_token(dumps[4].left);
      const std::string right_r = crop_token(dumps[4].right);
      const std::string left_swap = crop_token(dumps[5].left);
      const std::string right_swap = crop_token(dumps[5].right);

      if (left0 != "" || right0 != "") {
        fail_crop("initial filters already contained an application crop");
      } else if (left_b != "" || right_b != "") {
        fail_crop("Shift+B changed a crop state when one side is unsupported");
      } else if (dumps[1].left != dumps[0].left || dumps[1].right != dumps[0].right) {
        fail_crop("Shift+B changed filter chains");
      } else if (left_l != "crop=120:80:20:20") {
        fail_crop("Shift+L did not emit crop=120:80:20:20 in 320x180 crop-space");
      } else if (left_l == "crop=60:40:10:10") {
        fail_crop("Shift+L used dest-space 160x90 instead of crop-space");
      } else if (dumps[2].left.find("scale=160:90") == std::string::npos) {
        fail_crop("Shift+L lost the post-scale filter");
      } else if (right_l != "") {
        fail_crop("Shift+L changed the unsupported right crop");
      } else if (left_undo != "" || right_undo != "") {
        fail_crop("Backspace did not restore the pre-Shift+L crop state");
      } else if (left_r != "" || right_r != "") {
        fail_crop("Shift+R changed crop state on an unsupported right");
      } else if (left_swap != "" || right_swap != "") {
        fail_crop("swapped Shift+L changed crop state");
      } else {
        std::printf("PASS interactive-crop post-scale Shift+L and atomic Shift+B\n");
      }
    }
  } else if (scenario == Scenario::DynamicSwap) {
    const std::vector<FilterDump> dumps = parse_display_filters(output);
    auto fail_dynamic = [&](const char* message) {
      std::fprintf(stderr, "FAIL dynamic-swap: %s\n", message);
      for (size_t i = 0; i < dumps.size(); ++i) {
        std::fprintf(stderr, "dump %zu: aspect=%s swapped=%s window=%s content=%s ref_dar=%s visual_left=%s visual_right=%s\n", i, dumps[i].aspect.c_str(), dumps[i].swapped.c_str(), dumps[i].window.c_str(), dumps[i].content.c_str(),
                     dumps[i].ref_dar.c_str(), dumps[i].visual_left.c_str(), dumps[i].visual_right.c_str());
      }
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    };

    if (dumps.size() < 2) {
      fail_dynamic("expected Shift+X dumps before and after Swap");
    } else {
      const FilterDump& before = dumps[0];
      const FilterDump& after = dumps[1];
      WindowSize window_before{};
      WindowSize window_after{};
      ContentViewport content_before{};
      ContentViewport content_after{};
      int dar_before_num = 0;
      int dar_before_den = 0;
      int dar_after_num = 0;
      int dar_after_den = 0;
      const bool window_ok = parse_window_size(before.window, &window_before) && parse_window_size(after.window, &window_after);
      const bool content_ok = parse_content_viewport(before.content, &content_before) && parse_content_viewport(after.content, &content_after);
      const bool dar_ok = parse_dar(before.ref_dar, &dar_before_num, &dar_before_den) && parse_dar(after.ref_dar, &dar_after_num, &dar_after_den);
      const ContentViewport pillar_43 = window_ok ? viewport_for_aspect(window_before, 4.0F / 3.0F) : ContentViewport{};

      if (before.aspect != "dynamic" || after.aspect != "dynamic") {
        fail_dynamic("Shift+X did not report aspect=dynamic");
      } else if (before.swapped != "false" || after.swapped != "true") {
        fail_dynamic("Swap did not report swapped=false then swapped=true");
      } else if (before.visual_left != "320x240" || before.visual_right != "640x360") {
        fail_dynamic("pre-Swap visual sizes were not 4:3 left (320x240) and 16:9 right (640x360)");
      } else if (after.visual_left != before.visual_right || after.visual_right != before.visual_left) {
        fail_dynamic("Swap did not exchange visual_left/visual_right");
      } else if (!dar_ok) {
        fail_dynamic("Shift+X did not report ref_dar");
      } else if (!dar_equals(dar_before_num, dar_before_den, 4, 3)) {
        fail_dynamic("Dynamic reference was not the original left 4:3 input");
      } else if (dar_equals(dar_after_num, dar_after_den, 16, 9)) {
        fail_dynamic("Dynamic followed the visually-left 16:9 input after Swap");
      } else if (!dar_equals(dar_after_num, dar_after_den, 4, 3)) {
        fail_dynamic("Swap changed the Dynamic reference away from the original left 4:3 input");
      } else if (!window_ok || window_before.w != window_after.w || window_before.h != window_after.h) {
        fail_dynamic("Swap changed the OS window size");
      } else if (!(window_before.w * 3 > window_before.h * 4)) {
        fail_dynamic("window is not wider than 4:3, so letterboxing would not show a 16:9 policy change");
      } else if (!content_ok || !same_viewport(content_before, content_after)) {
        fail_dynamic("Swap changed the letterbox viewport");
      } else if (!same_viewport(content_before, pillar_43)) {
        fail_dynamic("letterbox viewport is not the 4:3 pillarbox of the original left input");
      } else {
        std::printf("PASS dynamic-swap kept ref_dar=%s and content=%s after Swap (window=%s)\n", before.ref_dar.c_str(), before.content.c_str(), before.window.c_str());
      }
    }
  }

  if (exit_code == EXIT_SUCCESS) {
    std::printf("PASS VideoCompare returned without throwing\n");
  }

  return exit_code;
}

void print_usage(const char* argv0) {
  std::fprintf(stderr, "usage:\n");
  std::fprintf(stderr, "  %s baseline LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s event-injection LEFT RIGHT0 RIGHT1\n", argv0);
  std::fprintf(stderr, "  %s seek LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s still-seek LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s sync-mismatch LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s multi-right-sync LEFT RIGHT0 RIGHT1\n", argv0);
  std::fprintf(stderr, "  %s frame-navigation LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s keyframe-navigation LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s keyframe-navigation-gop25 LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s keyframe-navigation-asymmetric LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s buffer-forward-only LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s buffer-pingpong LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s seek-burst-forward LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s seek-burst-mixed LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s crop-copy LEFT RIGHT0 RIGHT1 RIGHT2\n", argv0);
  std::fprintf(stderr, "  %s interactive-crop LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s dynamic-swap LEFT RIGHT\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  const std::string name = argv[1];
  std::vector<std::string> files;
  for (int i = 2; i < argc; ++i) {
    files.push_back(argv[i]);
  }

  try {
    if (name == "baseline") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::Baseline, files);
    }
    if (name == "event-injection") {
      if (files.size() != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::EventInjection, files);
    }
    if (name == "seek") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::Seek, files);
    }
    if (name == "still-seek") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::StillSeek, files);
    }
    if (name == "sync-mismatch") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::SyncMismatch, files);
    }
    if (name == "multi-right-sync") {
      if (files.size() != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::MultiRightSync, files);
    }
    if (name == "frame-navigation") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::FrameNavigation, files);
    }
    if (name == "keyframe-navigation") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::KeyframeNavigation, files);
    }
    if (name == "keyframe-navigation-gop25") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::KeyframeNavigationGop25, files);
    }
    if (name == "keyframe-navigation-asymmetric") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::KeyframeNavigationAsymmetric, files);
    }
    if (name == "keyframe-navigation-union-time-shift") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::KeyframeNavigationUnionTimeShift, files);
    }
    if (name == "buffer-forward-only") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::BufferForwardOnly, files);
    }
    if (name == "buffer-pingpong") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::BufferPingPong, files);
    }
    if (name == "seek-burst-forward") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::SeekBurstForward, files);
    }
    if (name == "seek-burst-mixed") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::SeekBurstMixed, files);
    }
    if (name == "crop-copy") {
      if (files.size() != 4) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::CropCopy, files);
    }
    if (name == "interactive-crop") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::InteractiveCrop, files);
    }
    if (name == "dynamic-swap") {
      if (files.size() != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      return run_scenario(Scenario::DynamicSwap, files);
    }
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "FAIL %s\n", exception.what());
    return EXIT_FAILURE;
  }

  print_usage(argv[0]);
  return EXIT_FAILURE;
}
