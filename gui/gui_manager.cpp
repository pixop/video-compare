#include "gui_manager.h"

#ifdef ENABLE_GUI
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#endif

GuiManager::GuiManager() {}

GuiManager::~GuiManager() {
  shutdown();
}

void GuiManager::init(SDL_Window* window, SDL_Renderer* renderer) {
#ifdef ENABLE_GUI
  if (initialized_) {
    return;
  }

  window_ = window;
  renderer_ = renderer;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  // Disable imgui.ini persistence for now — video-compare is a tool, not an editor.
  io.IniFilename = nullptr;

  // Dark colour scheme inspired by the HTML reference (teal/gold on dark background)
  ImGuiStyle& style = ImGui::GetStyle();
  ImGui::StyleColorsDark(&style);

  // Custom palette: deep teal background, gold accents
  ImVec4* colors = style.Colors;
  colors[ImGuiCol_WindowBg]         = ImVec4(0.008f, 0.039f, 0.051f, 0.94f);  // --bg #010a0c
  colors[ImGuiCol_TitleBg]          = ImVec4(0.008f, 0.102f, 0.129f, 1.00f);  // --sf #021a21
  colors[ImGuiCol_TitleBgActive]    = ImVec4(0.012f, 0.165f, 0.212f, 1.00f);  // --sh #032a36
  colors[ImGuiCol_FrameBg]          = ImVec4(0.008f, 0.039f, 0.051f, 1.00f);
  colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.012f, 0.165f, 0.212f, 1.00f);
  colors[ImGuiCol_FrameBgActive]    = ImVec4(0.000f, 0.420f, 0.459f, 0.67f);  // --p #006B75
  colors[ImGuiCol_Button]           = ImVec4(0.012f, 0.165f, 0.212f, 1.00f);
  colors[ImGuiCol_ButtonHovered]    = ImVec4(0.000f, 0.420f, 0.459f, 1.00f);
  colors[ImGuiCol_ButtonActive]     = ImVec4(1.000f, 0.843f, 0.000f, 1.00f);  // --y #FFD700
  colors[ImGuiCol_Header]           = ImVec4(0.000f, 0.420f, 0.459f, 0.31f);
  colors[ImGuiCol_HeaderHovered]    = ImVec4(0.000f, 0.420f, 0.459f, 0.80f);
  colors[ImGuiCol_HeaderActive]     = ImVec4(0.000f, 0.576f, 0.639f, 1.00f);  // --pl #0093A3
  colors[ImGuiCol_SliderGrab]       = ImVec4(0.000f, 0.576f, 0.639f, 1.00f);
  colors[ImGuiCol_SliderGrabActive] = ImVec4(1.000f, 0.843f, 0.000f, 1.00f);
  colors[ImGuiCol_CheckMark]        = ImVec4(1.000f, 0.843f, 0.000f, 1.00f);
  colors[ImGuiCol_Text]             = ImVec4(0.878f, 0.949f, 0.961f, 1.00f);  // --tx #e0f2f5
  colors[ImGuiCol_TextDisabled]     = ImVec4(0.500f, 0.600f, 0.620f, 1.00f);
  colors[ImGuiCol_Border]           = ImVec4(0.016f, 0.263f, 0.329f, 1.00f);  // --bo #044354
  colors[ImGuiCol_Separator]        = colors[ImGuiCol_Border];
  colors[ImGuiCol_Tab]              = ImVec4(0.008f, 0.102f, 0.129f, 1.00f);
  colors[ImGuiCol_TabHovered]       = ImVec4(0.000f, 0.576f, 0.639f, 0.80f);
  colors[ImGuiCol_TabSelected]      = ImVec4(0.000f, 0.420f, 0.459f, 1.00f);
  colors[ImGuiCol_PopupBg]          = ImVec4(0.008f, 0.059f, 0.071f, 0.94f);
  colors[ImGuiCol_ScrollbarBg]      = ImVec4(0.008f, 0.039f, 0.051f, 0.53f);
  colors[ImGuiCol_ScrollbarGrab]    = ImVec4(0.016f, 0.263f, 0.329f, 1.00f);

  style.WindowRounding   = 4.0f;
  style.FrameRounding    = 4.0f;
  style.GrabRounding     = 4.0f;
  style.TabRounding      = 4.0f;
  style.ScrollbarRounding = 4.0f;
  style.WindowBorderSize  = 1.0f;
  style.FrameBorderSize   = 1.0f;

  ImGui_ImplSDL2_InitForSDLRenderer(window_, renderer_);
  ImGui_ImplSDLRenderer2_Init(renderer_);

  initialized_ = true;
#else
  (void)window;
  (void)renderer;
#endif
}

void GuiManager::shutdown() {
#ifdef ENABLE_GUI
  if (!initialized_) {
    return;
  }

  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  initialized_ = false;
#endif
}

bool GuiManager::process_event(const SDL_Event& event) {
#ifdef ENABLE_GUI
  if (!initialized_) {
    return false;
  }
  ImGui_ImplSDL2_ProcessEvent(&event);
  // Return true if ImGui wants this event (mouse over widget or keyboard captured)
  const ImGuiIO& io = ImGui::GetIO();
  bool mouse_event = (event.type >= SDL_MOUSEMOTION && event.type <= SDL_MOUSEWHEEL);
  bool keyboard_event = (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP || event.type == SDL_TEXTINPUT);
  if (mouse_event && io.WantCaptureMouse) {
    return true;
  }
  if (keyboard_event && io.WantCaptureKeyboard) {
    return true;
  }
  return false;
#else
  (void)event;
  return false;
#endif
}

bool GuiManager::wants_capture_mouse() const {
#ifdef ENABLE_GUI
  if (!initialized_) {
    return false;
  }
  return ImGui::GetIO().WantCaptureMouse;
#else
  return false;
#endif
}

bool GuiManager::wants_capture_keyboard() const {
#ifdef ENABLE_GUI
  if (!initialized_) {
    return false;
  }
  return ImGui::GetIO().WantCaptureKeyboard;
#else
  return false;
#endif
}

void GuiManager::begin_frame() {
#ifdef ENABLE_GUI
  if (!initialized_) {
    return;
  }
  ImGui_ImplSDLRenderer2_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();
#endif
}

void GuiManager::end_frame() {
#ifdef ENABLE_GUI
  if (!initialized_) {
    return;
  }
  ImGui::Render();
  ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer_);
#endif
}
