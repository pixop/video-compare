#ifdef ENABLE_GUI
#include "gui_launcher.h"
#include "gui_manager.h"
#include "imgui.h"
#include "version.h"
#include "video_compare.h"
#include "side_aware_logger.h"
#include <SDL2/SDL.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sstream>
extern "C" {
#include <libavutil/dict.h>
}

// ── Helpers ────────────────────────────────────────────────────────────────

static void set_string(char* dst, size_t max, const std::string& src) {
  std::strncpy(dst, src.c_str(), max - 1);
  dst[max - 1] = '\0';
}

// ── Construction ───────────────────────────────────────────────────────────

GuiLauncher::GuiLauncher() {
  std::memset(left_path_, 0, sizeof(left_path_));
  std::memset(right_path_, 0, sizeof(right_path_));
  std::memset(left_filters_, 0, sizeof(left_filters_));
  std::memset(right_filters_, 0, sizeof(right_filters_));
  std::memset(time_shift_, 0, sizeof(time_shift_));
  std::memset(window_size_, 0, sizeof(window_size_));
}

GuiLauncher::~GuiLauncher() {
  if (gui_) {
    gui_->shutdown();
    delete gui_;
    gui_ = nullptr;
  }
  if (renderer_) {
    SDL_DestroyRenderer(renderer_);
  }
  if (window_) {
    SDL_DestroyWindow(window_);
  }
}

// ── Main loop ──────────────────────────────────────────────────────────────

int GuiLauncher::run() {
  // Initialize SDL (may already be initialized — SDL_Init is safe to call multiple times)
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
    std::cerr << "SDL_Init error: " << SDL_GetError() << std::endl;
    return -1;
  }

  window_ = SDL_CreateWindow(
      "video-compare — Launcher",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      780, 700,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window_) {
    std::cerr << "SDL_CreateWindow error: " << SDL_GetError() << std::endl;
    return -1;
  }

  renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer_) {
    std::cerr << "SDL_CreateRenderer error: " << SDL_GetError() << std::endl;
    return -1;
  }

  gui_ = new GuiManager();
  gui_->init(window_, renderer_);

  // Enable drag & drop
  SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

  // Main loop
  while (running_) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      gui_->process_event(event);

      if (event.type == SDL_QUIT) {
        running_ = false;
      }
      // Handle file drag & drop
      if (event.type == SDL_DROPFILE) {
        const char* dropped = event.drop.file;
        if (std::strlen(left_path_) == 0) {
          set_string(left_path_, sizeof(left_path_), dropped);
        } else if (std::strlen(right_path_) == 0) {
          set_string(right_path_, sizeof(right_path_), dropped);
        }
        SDL_free(event.drop.file);
      }
    }

    SDL_SetRenderDrawColor(renderer_, 1, 10, 12, 255);  // --bg #010a0c
    SDL_RenderClear(renderer_);

    gui_->begin_frame();
    draw_ui();
    gui_->end_frame();

    SDL_RenderPresent(renderer_);

    if (comparison_requested_) {
      comparison_requested_ = false;
      start_comparison();
    }
  }

  return 0;
}

// ── ImGui UI ───────────────────────────────────────────────────────────────

void GuiLauncher::draw_ui() {
  // Full-window ImGui panel
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("##launcher", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
               ImGuiWindowFlags_NoBringToFrontOnFocus);

  // Title
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.843f, 0.0f, 1.0f));  // gold
  ImGui::SetWindowFontScale(1.4f);
  ImGui::Text("video-compare");
  ImGui::SetWindowFontScale(1.0f);
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextDisabled("%s", VersionInfo::version.c_str());
  ImGui::Separator();
  ImGui::Spacing();

  // ── Left video ──────────────────────────────────────────────────────────
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.576f, 0.639f, 1.0f));  // teal label
  ImGui::Text("SOURCE A (Reference)");
  ImGui::PopStyleColor();

  ImGui::PushItemWidth(-1);
  ImGui::InputTextWithHint("##left_path", "Drag & drop or enter path to left video...", left_path_, sizeof(left_path_));
  ImGui::PopItemWidth();

  ImGui::PushItemWidth(-1);
  ImGui::InputTextWithHint("##left_filters", "FFmpeg filters (optional, e.g. crop=iw:ih-240)", left_filters_, sizeof(left_filters_));
  ImGui::PopItemWidth();
  ImGui::Spacing();

  // ── Right video ─────────────────────────────────────────────────────────
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.576f, 0.639f, 1.0f));
  ImGui::Text("SOURCE B (Encode)");
  ImGui::PopStyleColor();

  ImGui::PushItemWidth(-1);
  ImGui::InputTextWithHint("##right_path", "Drag & drop or enter path to right video...", right_path_, sizeof(right_path_));
  ImGui::PopItemWidth();

  ImGui::PushItemWidth(-1);
  ImGui::InputTextWithHint("##right_filters", "FFmpeg filters (optional, e.g. yadif,hqdn3d)", right_filters_, sizeof(right_filters_));
  ImGui::PopItemWidth();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // ── Settings section ────────────────────────────────────────────────────
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.576f, 0.639f, 1.0f));
  ImGui::Text("SETTINGS");
  ImGui::PopStyleColor();
  ImGui::Spacing();

  // Two-column layout
  float col_width = ImGui::GetContentRegionAvail().x * 0.5f;

  // Left column
  ImGui::BeginGroup();
  ImGui::PushItemWidth(col_width - 20);

  const char* mode_items[] = {"Split Screen", "Vertical Stack", "Horizontal Stack"};
  ImGui::Combo("Display Mode", &display_mode_, mode_items, 3);

  const char* loop_items[] = {"Off", "Forward Only", "Ping-Pong"};
  ImGui::Combo("Auto-Loop", &auto_loop_mode_, loop_items, 3);

  ImGui::SliderInt("Frame Buffer", &frame_buffer_size_, 1, 300, "%d frames");

  ImGui::InputTextWithHint("##timeshift", "Time Shift (e.g. 0.150 or x1.04+0.1)", time_shift_, sizeof(time_shift_));
  ImGui::InputTextWithHint("##winsize", "Window Size (e.g. 1280x720)", window_size_, sizeof(window_size_));

  ImGui::PopItemWidth();
  ImGui::EndGroup();

  ImGui::SameLine();

  // Right column — checkboxes
  ImGui::BeginGroup();
  ImGui::Checkbox("10-bit Color", &use_10_bpc_);
  ImGui::Checkbox("High DPI", &high_dpi_);
  ImGui::Checkbox("Fit Window to Display", &fit_window_);
  ImGui::Checkbox("Start in Subtraction Mode", &subtraction_mode_);
  ImGui::Checkbox("Fast Input Alignment", &fast_alignment_);
  ImGui::Checkbox("Bilinear Texture Filter", &bilinear_texture_);
  ImGui::EndGroup();

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // ── Status message ──────────────────────────────────────────────────────
  if (!status_message_.empty()) {
    if (status_is_error_) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.42f, 0.42f, 1.0f));
    } else {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.843f, 0.0f, 1.0f));
    }
    ImGui::TextWrapped("%s", status_message_.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }

  // ── Start button ────────────────────────────────────────────────────────
  float button_width = 220;
  float button_height = 42;
  float avail = ImGui::GetContentRegionAvail().x;
  ImGui::SetCursorPosX((avail - button_width) * 0.5f + ImGui::GetCursorStartPos().x);

  ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1.0f, 0.843f, 0.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(1.0f, 0.894f, 0.302f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.9f, 0.75f, 0.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text,           ImVec4(0.0f, 0.0f, 0.0f, 1.0f));

  if (ImGui::Button("START COMPARISON", ImVec2(button_width, button_height))) {
    comparison_requested_ = true;
  }

  ImGui::PopStyleColor(4);

  ImGui::Spacing();

  // ── Help text ───────────────────────────────────────────────────────────
  ImGui::TextDisabled("Tip: Drag & drop video files onto this window. First file -> Source A, second -> Source B.");
  ImGui::TextDisabled("Press Space during comparison to play/pause. Press H for full keyboard shortcuts.");

  ImGui::End();
}

// ── Build config and launch comparison ─────────────────────────────────────

void GuiLauncher::start_comparison() {
  // Validate inputs
  if (std::strlen(left_path_) == 0) {
    status_message_ = "Please select a left (reference) video file.";
    status_is_error_ = true;
    return;
  }
  if (std::strlen(right_path_) == 0) {
    status_message_ = "Please select a right (encode) video file.";
    status_is_error_ = true;
    return;
  }

  status_message_ = "Starting comparison...";
  status_is_error_ = false;

  try {
    VideoCompareConfig config;

    config.left.file_name = left_path_;

    InputVideo right_video{RIGHT, "Right"};
    right_video.file_name = right_path_;
    config.right_videos.push_back(right_video);

    // Filters
    if (std::strlen(left_filters_) > 0) {
      config.left.video_filters = left_filters_;
    }
    if (std::strlen(right_filters_) > 0) {
      config.right_videos[0].video_filters = right_filters_;
    }

    // Display mode
    switch (display_mode_) {
      case 0: config.display_mode = Display::Mode::Split; break;
      case 1: config.display_mode = Display::Mode::VStack; break;
      case 2: config.display_mode = Display::Mode::HStack; break;
    }

    // Auto-loop
    switch (auto_loop_mode_) {
      case 0: config.auto_loop_mode = Display::Loop::Off; break;
      case 1: config.auto_loop_mode = Display::Loop::ForwardOnly; break;
      case 2: config.auto_loop_mode = Display::Loop::PingPong; break;
    }

    // Options
    config.use_10_bpc = use_10_bpc_;
    config.high_dpi_allowed = high_dpi_;
    config.fit_window_to_usable_bounds = fit_window_;
    config.start_in_subtraction_mode = subtraction_mode_;
    config.fast_input_alignment = fast_alignment_;
    config.bilinear_texture_filtering = bilinear_texture_;
    config.frame_buffer_size = static_cast<size_t>(frame_buffer_size_);

    // Window size
    if (std::strlen(window_size_) > 0) {
      std::string ws(window_size_);
      auto x_pos = ws.find('x');
      if (x_pos != std::string::npos) {
        int w = x_pos > 0 ? std::stoi(ws.substr(0, x_pos)) : -1;
        int h = x_pos + 1 < ws.size() ? std::stoi(ws.substr(x_pos + 1)) : -1;
        config.window_size = std::make_tuple(w, h);
      }
    }

    // Time shift
    if (std::strlen(time_shift_) > 0) {
      // Store as simple offset for now; the full parse_time_shift is in main.cpp scope
      try {
        double offset = std::stod(time_shift_);
        config.time_shift.offset_ms = static_cast<int64_t>(offset * 1000.0);
      } catch (...) {
        status_message_ = "Cannot parse time shift value: " + std::string(time_shift_);
        status_is_error_ = true;
        return;
      }
    }

    // Default demuxer options
    AVDictionary* left_demuxer_opts = nullptr;
    av_dict_set(&left_demuxer_opts, "analyzeduration", "100000000", 0);
    av_dict_set(&left_demuxer_opts, "probesize", "100000000", 0);
    config.left.demuxer_options = left_demuxer_opts;

    AVDictionary* right_demuxer_opts = nullptr;
    av_dict_set(&right_demuxer_opts, "analyzeduration", "100000000", 0);
    av_dict_set(&right_demuxer_opts, "probesize", "100000000", 0);
    config.right_videos[0].demuxer_options = right_demuxer_opts;

    // Shut down launcher SDL resources before starting comparison
    // (comparison creates its own window)
    gui_->shutdown();
    delete gui_;
    gui_ = nullptr;
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
    SDL_DestroyWindow(window_);
    window_ = nullptr;

    av_log_set_callback(sa_av_log_callback);

    VideoCompare compare{config};
    compare();

    // After comparison finishes, stop the launcher loop
    running_ = false;

  } catch (const std::exception& e) {
    status_message_ = std::string("Error: ") + e.what();
    status_is_error_ = true;

    // Recreate launcher window if it was destroyed
    if (!window_) {
      window_ = SDL_CreateWindow(
          "video-compare — Launcher",
          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
          780, 700,
          SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
      renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
      gui_ = new GuiManager();
      gui_->init(window_, renderer_);
    }
  }
}

#endif  // ENABLE_GUI
