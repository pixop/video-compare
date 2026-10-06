#include "window_fit.h"
#include "zoom_transform.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using window_fit::FitWindowInput;
using window_fit::FitWindowResult;
using window_fit::WindowSizeChord;

static int failures = 0;

// Display::MIN_WINDOW_WIDTH / MIN_WINDOW_HEIGHT.
static constexpr int kMinW = 4;
static constexpr int kMinH = 1;

static constexpr float kUhdW = 3840.0F;
static constexpr float kUhdH = 2160.0F;

static void expect_true(const char* label, const bool got) {
  if (!got) {
    std::fprintf(stderr, "FAIL %s\n", label);
    failures++;
    return;
  }
  std::printf("PASS %s\n", label);
}

static void expect_int(const char* label, const int got, const int expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: expected %d, got %d\n", label, expected, got);
    failures++;
    return;
  }
  std::printf("PASS %s -> %d\n", label, got);
}

static void expect_chord(const char* label, const WindowSizeChord got, const WindowSizeChord expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: chord mismatch\n", label);
    failures++;
    return;
  }
  std::printf("PASS %s\n", label);
}

// Transcribes Display::update_content_window_layout() for a fixed content aspect.
struct ContentRect {
  int x;
  int y;
  int w;
  int h;
};

static ContentRect content_window_for_aspect(const int window_w, const int window_h, const float content_aspect, const bool stretch) {
  ContentRect content{0, 0, window_w, window_h};
  if (stretch) {
    return content;
  }

  const float window_aspect = static_cast<float>(window_w) / static_cast<float>(window_h);
  if (window_aspect > content_aspect) {
    content.h = window_h;
    content.w = std::max(1, static_cast<int>(std::lround(static_cast<float>(content.h) * content_aspect)));
    content.x = (window_w - content.w) / 2;
  } else {
    content.w = window_w;
    content.h = std::max(1, static_cast<int>(std::lround(static_cast<float>(content.w) / content_aspect)));
    content.y = (window_h - content.h) / 2;
  }
  return content;
}

static float layout_aspect(const float frame_aspect, const int mode_scale_num, const int mode_scale_den) {
  return frame_aspect * static_cast<float>(mode_scale_num) / static_cast<float>(mode_scale_den);
}

static FitWindowInput make_input(const float content_w, const float content_h, const float zoom, const float drawable_x, const float drawable_y, const int max_w, const int max_h) {
  FitWindowInput input{};
  input.content_w = content_w;
  input.content_h = content_h;
  input.zoom_factor = zoom;
  input.drawable_to_window_width_factor = drawable_x;
  input.drawable_to_window_height_factor = drawable_y;
  input.max_window_w = max_w;
  input.max_window_h = max_h;
  input.min_window_w = kMinW;
  input.min_window_h = kMinH;
  return input;
}

static FitWindowResult fit_stretch(const float window_w, const float window_h, const float zoom, const float drawable_x = 1.0F, const float drawable_y = 1.0F, const int max_w = 100000, const int max_h = 100000) {
  return window_fit::compute_fit_window_size(make_input(window_w, window_h, zoom, drawable_x, drawable_y, max_w, max_h));
}

static void expect_size(const char* label, const FitWindowResult& got, const int width, const int height, const bool capped) {
  if (got.width != width || got.height != height || got.capped_to_display != capped) {
    std::fprintf(stderr, "FAIL %s: expected %dx%d capped=%d, got %dx%d capped=%d\n", label, width, height, static_cast<int>(capped), got.width, got.height, static_cast<int>(got.capped_to_display));
    failures++;
    return;
  }
  std::printf("PASS %s -> %dx%d\n", label, got.width, got.height);
}

static void expect_aspect_close(const char* label, const FitWindowResult& got, const float src_w, const float src_h) {
  const float src_aspect = src_w / src_h;
  const float dst_aspect = static_cast<float>(got.width) / static_cast<float>(got.height);
  if (std::fabs(src_aspect - dst_aspect) > 0.002F) {
    std::fprintf(stderr, "FAIL %s: aspect %.5f vs source %.5f\n", label, dst_aspect, src_aspect);
    failures++;
    return;
  }
  std::printf("PASS %s aspect %.5f\n", label, dst_aspect);
}

static void test_modes_at_half_zoom() {
  // 3840x2160 clips. Stretch windows match the startup layout size.
  expect_size("split 0.5x", fit_stretch(kUhdW, kUhdH, 0.5F), 1920, 1080, false);
  expect_size("hstack 0.5x", fit_stretch(kUhdW * 2.0F, kUhdH, 0.5F), 3840, 1080, false);
  expect_size("vstack 0.5x", fit_stretch(kUhdW, kUhdH * 2.0F, 0.5F), 1920, 2160, false);
}

static void test_zoom_one_is_size_noop_then_normalize() {
  const FitWindowResult split = fit_stretch(kUhdW, kUhdH, 1.0F);
  const FitWindowResult hstack = fit_stretch(kUhdW * 2.0F, kUhdH, 1.0F);
  const FitWindowResult vstack = fit_stretch(kUhdW, kUhdH * 2.0F, 1.0F);
  expect_size("split 1.0x size no-op", split, 3840, 2160, false);
  expect_size("hstack 1.0x size no-op", hstack, 7680, 2160, false);
  expect_size("vstack 1.0x size no-op", vstack, 3840, 4320, false);

  // Pan is not an input. The same geometry is requested, then the view is recentered.
  const FitWindowResult again = fit_stretch(kUhdW * 2.0F, kUhdH, 1.0F);
  expect_true("pan does not change the fitted size", again.width == hstack.width && again.height == hstack.height);

  expect_true("zoom normalizes to 1", window_fit::zoom_factor_after_fit() == 1.0F);
  const auto pan = window_fit::pan_after_fit();
  expect_true("pan recenters to the origin", pan.move_x == 0.0F && pan.move_y == 0.0F);
  const auto center = zoom_transform::zoom_global_center_from_move_offset({pan.move_x, pan.move_y}, kUhdW, kUhdH);
  expect_true("recenter matches zoom global center", std::fabs(center.x - pan.center_x) < 1.0e-6F && std::fabs(center.y - pan.center_y) < 1.0e-6F);
}

static void test_zoom_above_one() {
  expect_size("split 2.0x grows the window", fit_stretch(1920.0F, 1080.0F, 2.0F), 3840, 2160, false);
  expect_size("hstack 2.0x grows the window", fit_stretch(7680.0F, 2160.0F, 2.0F), 15360, 4320, false);
  expect_true("zoom above 1 still normalizes to 1", window_fit::zoom_factor_after_fit() == 1.0F);
}

static void test_display_cap_preserves_aspect_and_zoom_one() {
  // 1920x1080 at 2x wants 3840x2160, which does not fit in 3000x2000.
  const FitWindowResult capped = fit_stretch(1920.0F, 1080.0F, 2.0F, 1.0F, 1.0F, 3000, 2000);
  expect_true("target larger than the display is capped", capped.capped_to_display);
  expect_true("capped size stays inside the usable box", capped.width <= 3000 && capped.height <= 2000);
  expect_true("capped size is smaller than the uncapped zoom", capped.width < 3840 && capped.height < 2160);
  expect_aspect_close("capped split keeps 16:9", capped, 16.0F, 9.0F);
  expect_int("capped width uses the limiting axis", capped.width, 3000);
  // 2160 * (3000/3840) is 1687.5. Fast-math may land on either side of the tie.
  expect_true("capped height stays on the 16:9 line", capped.height == 1687 || capped.height == 1688);

  // HStack 7680x2160 at 2x wants 15360x4320 against an 8000x2000 usable box.
  const FitWindowResult hstack = fit_stretch(7680.0F, 2160.0F, 2.0F, 1.0F, 1.0F, 8000, 2000);
  expect_true("hstack cap is flagged", hstack.capped_to_display);
  expect_true("hstack cap stays inside the usable box", hstack.width <= 8000 && hstack.height <= 2000);
  expect_aspect_close("capped hstack keeps layout aspect", hstack, 7680.0F, 2160.0F);
  expect_true("capped fit still uses zoom 1 rather than a second scale", window_fit::zoom_factor_after_fit() == 1.0F);

  // Exactly on the limit is not a cap.
  expect_size("exact usable size is not capped", fit_stretch(1920.0F, 1080.0F, 2.0F, 1.0F, 1.0F, 3840, 2160), 3840, 2160, false);

  // Zoom 1 is a size no-op only when that window already fits. An oversized
  // 100% window is still scaled down so it stays on the usable display.
  expect_size("zoom 1 larger than the display scales down", fit_stretch(7680.0F, 2160.0F, 1.0F, 1.0F, 1.0F, 3840, 2160), 3840, 1080, true);
}

static void test_letterboxed_aspect_modes() {
  // 16:9 window, 4:3 content: pillarbox. Fit uses the content viewport, not the full window.
  const ContentRect pillar = content_window_for_aspect(1920, 1080, 4.0F / 3.0F, false);
  expect_int("4:3 pillar content width", pillar.w, 1440);
  expect_int("4:3 pillar content height", pillar.h, 1080);
  expect_true("4:3 content is inset", pillar.x > 0 && pillar.y == 0);

  const FitWindowResult half = window_fit::compute_fit_window_size(make_input(static_cast<float>(pillar.w), static_cast<float>(pillar.h), 0.5F, 1.0F, 1.0F, 100000, 100000));
  expect_size("letterboxed 4:3 at 0.5x wraps the video", half, 720, 540, false);
  expect_true("letterbox fit is not half of the full window", !(half.width == 960 && half.height == 540));

  // 4:3 window, 16:9 content: bars above and below.
  const ContentRect letter = content_window_for_aspect(1440, 1080, 16.0F / 9.0F, false);
  expect_int("16:9 letter content width", letter.w, 1440);
  expect_int("16:9 letter content height", letter.h, 810);
  const FitWindowResult letter_half = window_fit::compute_fit_window_size(make_input(static_cast<float>(letter.w), static_cast<float>(letter.h), 0.5F, 1.0F, 1.0F, 100000, 100000));
  expect_size("letterboxed 16:9 at 0.5x wraps the video", letter_half, 720, 405, false);

  // HStack / VStack apply the layout multiplier before fitting the viewport.
  const int hstack_window_w = static_cast<int>(kUhdW * 2.0F);
  const int hstack_window_h = static_cast<int>(kUhdH);
  const ContentRect hstack_43 = content_window_for_aspect(hstack_window_w, hstack_window_h, layout_aspect(4.0F / 3.0F, 2, 1), false);
  expect_int("hstack 4:3 content width", hstack_43.w, 5760);
  expect_int("hstack 4:3 content height", hstack_43.h, 2160);
  const FitWindowResult hstack_fit = window_fit::compute_fit_window_size(make_input(static_cast<float>(hstack_43.w), static_cast<float>(hstack_43.h), 0.5F, 1.0F, 1.0F, 100000, 100000));
  expect_size("hstack 4:3 at 0.5x", hstack_fit, 2880, 1080, false);

  const ContentRect vstack_43 = content_window_for_aspect(static_cast<int>(kUhdW), static_cast<int>(kUhdH * 2.0F), layout_aspect(4.0F / 3.0F, 1, 2), false);
  expect_int("vstack 4:3 content width", vstack_43.w, 2880);
  expect_int("vstack 4:3 content height", vstack_43.h, 4320);
  const FitWindowResult vstack_fit = window_fit::compute_fit_window_size(make_input(static_cast<float>(vstack_43.w), static_cast<float>(vstack_43.h), 0.5F, 1.0F, 1.0F, 100000, 100000));
  expect_size("vstack 4:3 at 0.5x", vstack_fit, 1440, 2160, false);

  // Zoom 1 still drops the bars: the displayed video is the content viewport.
  const FitWindowResult pillar_full = window_fit::compute_fit_window_size(make_input(static_cast<float>(pillar.w), static_cast<float>(pillar.h), 1.0F, 1.0F, 1.0F, 100000, 100000));
  expect_size("zoom 1 pillarbox shrinks to the content", pillar_full, pillar.w, pillar.h, false);
}

static void test_high_dpi_uses_logical_window_coordinates() {
  // High-DPI HStack of 3840x2160: logical window is half the layout (3840x1080)
  // and the drawable is 2x. Video layout pixels are 7680x2160. At 0.5x the
  // logical target is 1920x540, not 3840x1080 (drawable or video pixels).
  const float content_w = 3840.0F;
  const float content_h = 1080.0F;
  const float zoom = 0.5F;
  const float dpi = 2.0F;
  const FitWindowResult fitted = fit_stretch(content_w, content_h, zoom, dpi, dpi);
  expect_size("high-DPI hstack 0.5x stays in logical pixels", fitted, 1920, 540, false);

  const float physical_before_w = content_w * dpi * zoom;
  const float physical_before_h = content_h * dpi * zoom;
  const float physical_after_w = static_cast<float>(fitted.width) * dpi * window_fit::zoom_factor_after_fit();
  const float physical_after_h = static_cast<float>(fitted.height) * dpi * window_fit::zoom_factor_after_fit();
  expect_true("high-DPI physical width is unchanged", std::fabs(physical_before_w - physical_after_w) < 0.5F);
  expect_true("high-DPI physical height is unchanged", std::fabs(physical_before_h - physical_after_h) < 0.5F);
  expect_true("result is not the video-pixel layout", !(fitted.width == 3840 && fitted.height == 1080));

  // Non-uniform drawable scale still cancels back to content * zoom.
  const FitWindowResult nonuniform = fit_stretch(1000.0F, 500.0F, 0.5F, 2.0F, 3.0F);
  expect_size("non-uniform DPI cancels to logical content * zoom", nonuniform, 500, 250, false);
  const float nu_before_w = 1000.0F * 2.0F * 0.5F;
  const float nu_before_h = 500.0F * 3.0F * 0.5F;
  expect_true("non-uniform physical width is unchanged", std::fabs(nu_before_w - static_cast<float>(nonuniform.width) * 2.0F) < 0.5F);
  expect_true("non-uniform physical height is unchanged", std::fabs(nu_before_h - static_cast<float>(nonuniform.height) * 3.0F) < 0.5F);
}

static void test_usable_bounds_match_startup_allowance() {
  const auto limits = window_fit::usable_window_limits(1920, 1080, kMinW, kMinH);
  expect_int("usable width subtracts the frame border", limits.max_w, 1920 - window_fit::kFrameBorderWidth);
  expect_int("usable height subtracts the title border", limits.max_h, 1080 - window_fit::kFrameBorderHeight);
  expect_int("frame border width matches -W", window_fit::kFrameBorderWidth, 10);
#ifdef __linux__
  expect_int("linux title border matches -W", window_fit::kFrameBorderHeight, 40);
#else
  expect_int("title border matches -W", window_fit::kFrameBorderHeight, 34);
#endif

  const auto tiny = window_fit::usable_window_limits(8, 8, kMinW, kMinH);
  expect_true("tiny displays do not produce a negative cap", tiny.max_w >= kMinW && tiny.max_h >= kMinH);
}

static void expect_inside_usable_display(const char* label, const window_fit::WindowPosition& pos, const window_fit::DisplayBounds& bounds, const int window_w, const int window_h) {
  const bool inside = pos.x >= bounds.x && pos.y >= bounds.y && pos.x + window_w <= bounds.x + bounds.w && pos.y + window_h <= bounds.y + bounds.h;
  if (!inside) {
    std::fprintf(stderr, "FAIL %s: window %d,%d %dx%d outside usable %d,%d %dx%d\n", label, pos.x, pos.y, window_w, window_h, bounds.x, bounds.y, bounds.w, bounds.h);
    failures++;
    return;
  }
  std::printf("PASS %s\n", label);
}

static void test_centers_fitted_window_on_usable_display() {
  // Numbers are the `-W` placement: usable size minus the frame/title allowance,
  // then the same integer centering, including the extra title offset.
  const window_fit::DisplayBounds primary{0, 0, 1920, 1080};
  const auto primary_pos = window_fit::centered_window_position(primary, 800, 450, kMinW, kMinH);
  expect_int("primary display centered x", primary_pos.x, 560);
#ifdef __linux__
  expect_int("primary display centered y", primary_pos.y, 301);
#else
  expect_int("primary display centered y", primary_pos.y, 325);
#endif
  expect_inside_usable_display("primary fitted window is fully on screen", primary_pos, primary, 800, 450);

  // A display that does not start at the desktop origin. The old off-screen
  // window position is not an input; the result is centered on this display.
  const window_fit::DisplayBounds secondary{1920, 100, 2560, 1440};
  const auto secondary_pos = window_fit::centered_window_position(secondary, 1280, 720, kMinW, kMinH);
  expect_int("secondary display centered x", secondary_pos.x, 2560);
#ifdef __linux__
  expect_int("secondary display centered y", secondary_pos.y, 446);
#else
  expect_int("secondary display centered y", secondary_pos.y, 470);
#endif
  expect_inside_usable_display("secondary fitted window is fully on screen", secondary_pos, secondary, 1280, 720);

  // Oversized window brought back: 3840x2160 desktop, fitted 1920x1080 view.
  const window_fit::DisplayBounds desktop{0, 0, 3840, 2160};
  const auto brought_back = window_fit::centered_window_position(desktop, 1920, 1080, kMinW, kMinH);
  expect_int("oversized window recenters x", brought_back.x, 960);
#ifdef __linux__
  expect_int("oversized window recenters y", brought_back.y, 526);
#else
  expect_int("oversized window recenters y", brought_back.y, 550);
#endif
  expect_inside_usable_display("oversized window is brought fully on screen", brought_back, desktop, 1920, 1080);

  // Odd remainder truncates the same way as the startup integer division.
  const auto odd = window_fit::centered_window_position(primary, 801, 450, kMinW, kMinH);
  expect_int("odd width uses -W integer division", odd.x, 559);
}

static void test_existing_window_size_chords_and_fullscreen() {
  // ctrl, shift, alt, gui
  expect_chord("plain W fits", window_fit::window_size_chord(false, false, false, false), WindowSizeChord::FitToVideoView);
  expect_chord("Shift+W restores saved", window_fit::window_size_chord(false, true, false, false), WindowSizeChord::RestoreSaved);
  expect_chord("Ctrl+W restores startup", window_fit::window_size_chord(true, false, false, false), WindowSizeChord::RestoreStartup);
  expect_chord("Ctrl+Shift+W saves", window_fit::window_size_chord(true, true, false, false), WindowSizeChord::SaveCurrent);
  expect_chord("Alt+W does not fit", window_fit::window_size_chord(false, false, true, false), WindowSizeChord::None);
  expect_chord("Cmd/GUI+W does not fit", window_fit::window_size_chord(false, false, false, true), WindowSizeChord::None);
  expect_chord("Alt+Shift+W is not a window-size binding", window_fit::window_size_chord(false, true, true, false), WindowSizeChord::None);
  expect_chord("Cmd+Ctrl+W is not Ctrl+W", window_fit::window_size_chord(true, false, false, true), WindowSizeChord::None);

  auto surface = [](const bool native_fullscreen, const bool borderless, const bool maximized, const bool fullscreen_like) {
    window_fit::FitSurfaceState state;
    state.native_fullscreen = native_fullscreen;
    state.borderless = borderless;
    state.maximized = maximized;
    state.fullscreen_like = fullscreen_like;
    return window_fit::fit_surface_action(state);
  };

  expect_true("native fullscreen ignores fit", surface(true, false, false, true) == window_fit::FitSurfaceAction::Ignore);
  expect_true("native fullscreen plus maximized ignores fit", surface(true, false, true, true) == window_fit::FitSurfaceAction::Ignore);
  expect_true("borderless fullscreen-like surface ignores fit", surface(false, true, false, true) == window_fit::FitSurfaceAction::Ignore);
  expect_true("borderless maximized fullscreen-like surface ignores fit", surface(false, true, true, true) == window_fit::FitSurfaceAction::Ignore);
  expect_true("ordinary maximized fullscreen-like window is restored then fitted", surface(false, false, true, true) == window_fit::FitSurfaceAction::UnmaximizeThenFit);
  expect_true("ordinary maximized window is restored then fitted", surface(false, false, true, false) == window_fit::FitSurfaceAction::UnmaximizeThenFit);
  expect_true("ordinary window is fitted", surface(false, false, false, false) == window_fit::FitSurfaceAction::Fit);
}

int main() {
  test_modes_at_half_zoom();
  test_zoom_one_is_size_noop_then_normalize();
  test_zoom_above_one();
  test_display_cap_preserves_aspect_and_zoom_one();
  test_letterboxed_aspect_modes();
  test_high_dpi_uses_logical_window_coordinates();
  test_usable_bounds_match_startup_allowance();
  test_centers_fitted_window_on_usable_display();
  test_existing_window_size_chords_and_fullscreen();

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }

  std::printf("All window fit tests passed\n");
  return EXIT_SUCCESS;
}
