#define SDL_MAIN_HANDLED
#include "video_compare.h"
#include <SDL2/SDL.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

enum class Scenario { Baseline, EventInjection, Seek };

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

void run_event_script(const Scenario scenario, std::atomic<bool>& finished) {
  std::thread watchdog([&finished]() {
    for (int i = 0; i < 120 && !finished.load(); ++i) {
      sleep_ms(100);
    }
    if (!finished.load()) {
      std::fprintf(stderr, "watchdog: pushing SDL_QUIT after 12s deadline\n");
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
  for (const std::string& file : files) {
    require_readable_file(file.c_str());
  }

  prepare_dummy_software_sdl();

  const VideoCompareConfig config = make_config(files);

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
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "FAIL %s\n", exception.what());
    return EXIT_FAILURE;
  }

  print_usage(argv[0]);
  return EXIT_FAILURE;
}
