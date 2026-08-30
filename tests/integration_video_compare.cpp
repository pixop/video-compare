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

enum class Scenario { Baseline, EventInjection };

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

void push_keydown(const SDL_Keycode key, const SDL_Scancode scancode, const Uint32 window_id) {
  SDL_Event event{};
  event.type = SDL_KEYDOWN;
  event.key.type = SDL_KEYDOWN;
  event.key.timestamp = SDL_GetTicks();
  event.key.windowID = window_id;
  event.key.state = SDL_PRESSED;
  event.key.repeat = 0;
  event.key.keysym.scancode = scancode;
  event.key.keysym.sym = key;
  event.key.keysym.mod = KMOD_NONE;
  if (SDL_PushEvent(&event) < 0) {
    std::fprintf(stderr, "SDL_PushEvent(KEYDOWN) failed: %s\n", SDL_GetError());
  }
}

void run_event_script(const Scenario scenario, std::atomic<bool>& finished) {
  std::thread watchdog([&finished]() {
    for (int i = 0; i < 80 && !finished.load(); ++i) {
      sleep_ms(100);
    }
    if (!finished.load()) {
      std::fprintf(stderr, "watchdog: pushing SDL_QUIT after 8s deadline\n");
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
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "FAIL %s\n", exception.what());
    return EXIT_FAILURE;
  }

  print_usage(argv[0]);
  return EXIT_FAILURE;
}
