#define SDL_MAIN_HANDLED
#include "video_compare.h"
#include <SDL2/SDL.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits.h>
#include <memory>
#include <sstream>
#include <string>
#ifdef _WIN32
#include <stdlib.h>
#endif
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

enum class Scenario { Baseline, EventInjection, Seek, StillSeek, SyncMismatch, MultiRightSync, FrameNavigation, BufferForwardOnly, BufferPingPong, SeekBurstForward, SeekBurstMixed };

std::atomic<int> events_pushed{0};
std::atomic<bool> watchdog_fired{false};

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
    case Scenario::BufferForwardOnly:
      return "buffer-forward-only";
    case Scenario::BufferPingPong:
      return "buffer-pingpong";
    case Scenario::SeekBurstForward:
      return "seek-burst-forward";
    case Scenario::SeekBurstMixed:
      return "seek-burst-mixed";
  }
  return "unknown";
}

const char* stress_iteration_label() {
  const char* value = std::getenv("STRESS_ITERATION");
  return (value != nullptr && value[0] != '\0') ? value : "?";
}

int watchdog_ticks_for(const Scenario scenario) {
  // 100 ms per tick. Stress seeks serialize on drain/restart; 12 s is too tight.
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

void print_pts_stdout(const char* label, const std::vector<double>& times) {
  std::printf("%s", label);
  for (size_t i = 0; i < times.size(); ++i) {
    std::printf("%s%.3f", i == 0 ? "" : " ", times[i]);
  }
  std::printf("\n");
}

std::vector<double> copied_positions(const std::string& output) {
  std::vector<double> times;
  const std::string prefix = "Copied to clipboard: ";
  size_t pos = 0;
  while ((pos = output.find(prefix, pos)) != std::string::npos) {
    pos += prefix.size();
    const size_t end = output.find_first_of("\r\n", pos);
    const std::string token = output.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    double seconds = 0.0;
    if (parse_hhmmss(token, &seconds)) {
      times.push_back(seconds);
    }
  }
  return times;
}

void push_relative_seek(const int direction) {
  if (direction > 0) {
    push_keydown(SDLK_RIGHT, SDL_SCANCODE_RIGHT, 0);
  } else {
    push_keydown(SDLK_LEFT, SDL_SCANCODE_LEFT, 0);
  }
}

void print_stress_failure(const Scenario scenario, const char* message, const std::vector<double>& times, const double expected_storm) {
  std::fprintf(stderr, "FAIL %s iteration=%s events_pushed=%d watchdog=%s", scenario_name(scenario), stress_iteration_label(), events_pushed.load(),
               watchdog_fired.load() ? "fired" : "idle");
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
      std::fprintf(stderr, "watchdog: pushing SDL_QUIT after %ds deadline (%s iteration=%s)\n", watchdog_ticks / 10, scenario_name(scenario),
                   stress_iteration_label());
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
      sleep_ms(1500);
    }

    push_copy_timestamp();
    sleep_ms(300);
    push_keydown(SDLK_d, SDL_SCANCODE_D, 0, KMOD_SHIFT);
    sleep_ms(500);
    push_copy_timestamp();
    sleep_ms(200);
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
  if (scenario == Scenario::SyncMismatch || scenario == Scenario::MultiRightSync) {
    // Auto fps= would otherwise lift 25 fps to 30 and hide the sync path.
    config.disable_auto_filters = true;
  }
  if (scenario == Scenario::BufferForwardOnly || scenario == Scenario::BufferPingPong) {
    // 3 slots: newest / middle / oldest. Smallest history that still has
    // an interior frame plus both endpoints for wrap and bounce.
    config.frame_buffer_size = 3;
  }

  std::ostringstream captured_out;
  std::streambuf* const old_out = std::cout.rdbuf(captured_out.rdbuf());

  std::atomic<bool> finished{false};
  std::thread helper;
  int exit_code = EXIT_SUCCESS;

  try {
    VideoCompare video_compare(config);
    helper = std::thread(run_event_script, scenario, std::ref(finished));
    video_compare();
  } catch (const std::exception& exception) {
    finished.store(true);
    push_quit();
    std::cout.rdbuf(old_out);
    if (helper.joinable()) {
      helper.join();
    }
    std::fprintf(stderr, "FAIL VideoCompare threw: %s\n", exception.what());
    return EXIT_FAILURE;
  }

  std::cout.rdbuf(old_out);
  if (helper.joinable()) {
    helper.join();
  }

  const std::string output = captured_out.str();
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
    if (times.size() < 3) {
      std::fprintf(stderr, "FAIL seek expected 3 copied timestamps, got %zu\n", times.size());
      std::fprintf(stderr, "captured stdout:\n%s\n", output.c_str());
      exit_code = EXIT_FAILURE;
    } else {
      const double forward = times[1] - times[0];
      const double backward = times[1] - times[2];
      std::printf("seek timestamps: before=%.3f after+1s=%.3f after-1s=%.3f\n", times[0], times[1], times[2]);
      // Intra mpeg4 should land near ±1s. Allow decoder/PTS slack without
      // accepting a no-op (identical timestamps).
      if (forward < 0.40 || forward > 1.75) {
        std::fprintf(stderr, "FAIL forward seek delta %.3f not in [0.40, 1.75]\n", forward);
        exit_code = EXIT_FAILURE;
      } else if (backward < 0.40 || backward > 1.75) {
        std::fprintf(stderr, "FAIL backward seek delta %.3f not in [0.40, 1.75]\n", backward);
        exit_code = EXIT_FAILURE;
      } else {
        std::printf("PASS keyboard seek moved presented PTS forward then backward\n");
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
  std::fprintf(stderr, "  %s buffer-forward-only LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s buffer-pingpong LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s seek-burst-forward LEFT RIGHT\n", argv0);
  std::fprintf(stderr, "  %s seek-burst-mixed LEFT RIGHT\n", argv0);
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
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "FAIL %s\n", exception.what());
    return EXIT_FAILURE;
  }

  print_usage(argv[0]);
  return EXIT_FAILURE;
}
