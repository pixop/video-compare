#include "../zoom_transform.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using zoom_transform::ZoomRectState;
using zoom_transform::ZoomVec2;

static int failures = 0;

enum class Mode { Split, HStack, VStack };

// --pre-fix only. Not part of the production API.
enum class OriginPolicy {
  FrameCenter,
  LayoutCenter
};

struct ContentWindow {
  float x;
  float y;
  float w;
  float h;
};

struct Frame {
  float width;
  float height;
};

static constexpr float kTol = 1e-3F;
static constexpr Frame kHD{1920.0F, 1080.0F};

static const char* mode_name(const Mode mode) {
  switch (mode) {
    case Mode::HStack:
      return "HStack";
    case Mode::VStack:
      return "VStack";
    case Mode::Split:
    default:
      return "Split";
  }
}

static float layout_width(const Mode mode, const Frame& frame) {
  return frame.width * ((mode == Mode::HStack) ? 2.0F : 1.0F);
}

static float layout_height(const Mode mode, const Frame& frame) {
  return frame.height * ((mode == Mode::VStack) ? 2.0F : 1.0F);
}

static ZoomVec2 layout_size(const Mode mode, const Frame& frame) {
  return {layout_width(mode, frame), layout_height(mode, frame)};
}

// Transcribes Display::update_content_window_layout() for non-Stretch fitting.
static ContentWindow fit_content_window(const float window_w, const float window_h, const float content_aspect) {
  const float window_aspect = window_w / window_h;
  ContentWindow content{0.0F, 0.0F, window_w, window_h};

  if (window_aspect > content_aspect) {
    content.h = window_h;
    content.w = std::round(content.h * content_aspect);
    content.x = (window_w - content.w) / 2.0F;
  } else {
    content.w = window_w;
    content.h = std::round(content.w / content_aspect);
    content.y = (window_h - content.h) / 2.0F;
  }

  return content;
}

// Transcribes the mouse-wheel zoom_point conversion in Display::handle_event:
//   (mouse - content_window origin) * video_to_window factor
static ZoomVec2 window_to_layout(const ZoomVec2 window_pos, const ContentWindow& content, const ZoomVec2 layout) {
  return {(window_pos.x - content.x) * (layout.x / content.w), (window_pos.y - content.y) * (layout.y / content.h)};
}

static ZoomVec2 layout_to_window(const ZoomVec2 layout_pos, const ContentWindow& content, const ZoomVec2 layout) {
  return {content.x + layout_pos.x * (content.w / layout.x), content.y + layout_pos.y * (content.h / layout.y)};
}

static ZoomRectState zoom_rect_from_move_offset(const ZoomVec2 move_offset, const float zoom_factor, const Frame& frame) {
  const auto center = zoom_transform::zoom_global_center_from_move_offset(move_offset, frame.width, frame.height);
  return zoom_transform::compute_zoom_rect_state(center, zoom_factor, frame.width, frame.height);
}

static ZoomVec2 compute_offset(const ZoomVec2 move_offset, const float current_zoom, const ZoomVec2 zoom_point, const float new_zoom, const Frame& frame, const Mode mode, const OriginPolicy policy) {
  if (policy == OriginPolicy::FrameCenter) {
    return zoom_transform::compute_zoom_move_offset(move_offset, current_zoom, zoom_point, new_zoom, frame.width, frame.height);
  }

  const ZoomVec2 transform_origin{layout_width(mode, frame) * 0.5F, layout_height(mode, frame) * 0.5F};
  const float zoom_factor_change = new_zoom / current_zoom;
  return {move_offset.x - (transform_origin.x + move_offset.x - zoom_point.x) * (1.0F - zoom_factor_change), move_offset.y - (transform_origin.y + move_offset.y - zoom_point.y) * (1.0F - zoom_factor_change)};
}

static bool near(const float got, const float expected) {
  return std::fabs(got - expected) <= kTol;
}

static bool near_vec(const ZoomVec2& got, const ZoomVec2& expected) {
  return near(got.x, expected.x) && near(got.y, expected.y);
}

static void expect_near(const char* label, const float got, const float expected) {
  if (!near(got, expected)) {
    std::fprintf(stderr, "FAIL %s: expected %.6f, got %.6f\n", label, expected, got);
    failures++;
    return;
  }
  std::printf("PASS %s -> %.6f\n", label, got);
}

static void expect_vec(const char* label, const ZoomVec2& got, const ZoomVec2& expected) {
  if (!near_vec(got, expected)) {
    std::fprintf(stderr, "FAIL %s: expected (%.6f, %.6f), got (%.6f, %.6f)\n", label, expected.x, expected.y, got.x, got.y);
    failures++;
    return;
  }
  std::printf("PASS %s -> (%.6f, %.6f)\n", label, got.x, got.y);
}

// Invariant against Display's rendering path:
//   update_move_offset -> compute_zoom_rect -> video_to_zoom_space
//   and the zoom_rect inverse used by window_to_video_position.
static void expect_focal_point_stable(const char* label, const ZoomVec2 cursor, const ZoomVec2 move_offset, const float current_zoom, const float new_zoom, const Frame& frame, const Mode mode, const OriginPolicy policy) {
  const ZoomRectState rect_before = zoom_rect_from_move_offset(move_offset, current_zoom, frame);
  const ZoomVec2 source_before = zoom_transform::zoom_space_to_video_point(cursor, rect_before, frame.width, frame.height);

  const ZoomVec2 new_offset = compute_offset(move_offset, current_zoom, cursor, new_zoom, frame, mode, policy);
  const ZoomRectState rect_after = zoom_rect_from_move_offset(new_offset, new_zoom, frame);
  const ZoomVec2 source_after = zoom_transform::zoom_space_to_video_point(cursor, rect_after, frame.width, frame.height);
  const ZoomVec2 mapped_after = zoom_transform::video_point_to_zoom_space(source_before, rect_after);

  if (!near_vec(source_before, source_after) || !near_vec(mapped_after, cursor)) {
    std::fprintf(stderr, "FAIL %s: source before (%.6f, %.6f) after (%.6f, %.6f), mapped after (%.6f, %.6f) focal (%.6f, %.6f)\n", label, source_before.x, source_before.y, source_after.x, source_after.y, mapped_after.x, mapped_after.y,
                 cursor.x, cursor.y);
    failures++;
    return;
  }
  std::printf("PASS %s\n", label);
}

static std::string make_label(const char* prefix, const Mode mode, const char* detail) {
  return std::string(prefix) + " " + mode_name(mode) + " " + detail;
}

static void test_split_native(const OriginPolicy policy) {
  const Frame frame = kHD;
  const Mode mode = Mode::Split;
  const ZoomVec2 zero{0.0F, 0.0F};

  expect_focal_point_stable(make_label("1", mode, "native wheel focal point at frame center").c_str(), {frame.width * 0.5F, frame.height * 0.5F}, zero, 1.0F, 2.0F, frame, mode, policy);
  expect_focal_point_stable(make_label("1", mode, "native wheel focal point off-center").c_str(), {320.0F, 800.0F}, zero, 1.0F, 2.0F, frame, mode, policy);
}

static void test_split_letterboxed(const OriginPolicy policy) {
  const Frame frame = kHD;
  const Mode mode = Mode::Split;
  const ZoomVec2 layout = layout_size(mode, frame);
  const ContentWindow content = fit_content_window(1920.0F, 1440.0F, layout.x / layout.y);
  const ZoomVec2 zero{0.0F, 0.0F};

  if (!(content.y > 0.0F && near(content.x, 0.0F))) {
    std::fprintf(stderr, "FAIL 2 Split letterbox fixture: expected top/bottom bars, got x=%.1f y=%.1f w=%.1f h=%.1f\n", content.x, content.y, content.w, content.h);
    failures++;
    return;
  }
  std::printf("PASS 2 Split letterbox fixture -> content (%.1f, %.1f) %.1fx%.1f\n", content.x, content.y, content.w, content.h);

  const ZoomVec2 visual_center_window{content.x + content.w * 0.5F, content.y + content.h * 0.5F};
  const ZoomVec2 visual_center_layout = window_to_layout(visual_center_window, content, layout);
  expect_vec("2 Split letterbox window center maps to frame-center focal point", visual_center_layout, {frame.width * 0.5F, frame.height * 0.5F});

  const ZoomVec2 new_at_center = compute_offset(zero, 1.0F, visual_center_layout, 2.0F, frame, mode, policy);
  expect_vec("2 Split letterbox zoom at content-center focal point does not pan", new_at_center, zero);

  const ZoomVec2 off_center_layout{400.0F, 200.0F};
  const ZoomVec2 off_center_window = layout_to_window(off_center_layout, content, layout);
  const ZoomVec2 off_center_roundtrip = window_to_layout(off_center_window, content, layout);
  expect_vec("2 Split letterbox window conversion subtracts content_window origin", off_center_roundtrip, off_center_layout);
  expect_focal_point_stable("2 Split letterbox wheel focal point off-center", off_center_layout, zero, 1.0F, 2.0F, frame, mode, policy);

  expect_near("2 Split letterbox transform origin does not include content_window.y", new_at_center.y, 0.0F);
}

static void test_hstack_native(const OriginPolicy policy) {
  const Frame frame = kHD;
  const Mode mode = Mode::HStack;
  const ZoomVec2 zero{0.0F, 0.0F};
  const float w = frame.width;
  const float h = frame.height;

  const ZoomVec2 left_center{w * 0.5F, h * 0.5F};
  const ZoomVec2 seam{w, h * 0.5F};
  const ZoomVec2 right_center{w * 1.5F, h * 0.5F};
  const ZoomVec2 right_tile{w * 1.75F, h * 0.25F};

  expect_focal_point_stable("3 HStack native wheel focal point at left-frame center", left_center, zero, 1.0F, 2.0F, frame, mode, policy);
  expect_focal_point_stable("3 HStack native wheel focal point at seam", seam, zero, 1.0F, 2.0F, frame, mode, policy);
  expect_focal_point_stable("3 HStack native wheel focal point at right-frame center", right_center, zero, 1.0F, 2.0F, frame, mode, policy);
  expect_focal_point_stable("3 HStack native wheel focal point on right tile x>W", right_tile, zero, 1.0F, 2.0F, frame, mode, policy);

  const ZoomVec2 left_offset = compute_offset(zero, 1.0F, left_center, 2.0F, frame, mode, policy);
  expect_near("3 HStack canonical left-center offset.x", left_offset.x, 0.0F);
  expect_near("3 HStack canonical left-center offset.y", left_offset.y, 0.0F);

  const ZoomVec2 seam_offset = compute_offset(zero, 1.0F, seam, 2.0F, frame, mode, policy);
  expect_near("3 HStack canonical seam offset.x", seam_offset.x, -960.0F);
  expect_near("3 HStack canonical seam offset.y", seam_offset.y, 0.0F);
}

static void test_hstack_letterboxed(const OriginPolicy policy) {
  const Frame frame = kHD;
  const Mode mode = Mode::HStack;
  const ZoomVec2 layout = layout_size(mode, frame);
  const ContentWindow content = fit_content_window(1920.0F, 1080.0F, layout.x / layout.y);
  const ZoomVec2 zero{0.0F, 0.0F};
  const float w = frame.width;
  const float h = frame.height;

  if (!(content.y > 0.0F && near(content.x, 0.0F))) {
    std::fprintf(stderr, "FAIL 4 HStack letterbox fixture: expected top/bottom bars, got x=%.1f y=%.1f w=%.1f h=%.1f\n", content.x, content.y, content.w, content.h);
    failures++;
    return;
  }
  std::printf("PASS 4 HStack letterbox fixture -> content (%.1f, %.1f) %.1fx%.1f\n", content.x, content.y, content.w, content.h);

  const ZoomVec2 left_center{w * 0.5F, h * 0.5F};
  const ZoomVec2 seam{w, h * 0.5F};
  const ZoomVec2 right_center{w * 1.5F, h * 0.5F};

  expect_vec("4 HStack letterbox left-center window maps back", window_to_layout(layout_to_window(left_center, content, layout), content, layout), left_center);

  expect_focal_point_stable("4 HStack letterbox wheel focal point at left-frame center", left_center, zero, 1.0F, 2.0F, frame, mode, policy);
  expect_focal_point_stable("4 HStack letterbox wheel focal point at seam", seam, zero, 1.0F, 2.0F, frame, mode, policy);
  expect_focal_point_stable("4 HStack letterbox wheel focal point at right-frame center", right_center, zero, 1.0F, 2.0F, frame, mode, policy);

  const ZoomVec2 left_offset = compute_offset(zero, 1.0F, left_center, 2.0F, frame, mode, policy);
  const ZoomVec2 seam_offset = compute_offset(zero, 1.0F, seam, 2.0F, frame, mode, policy);
  const ZoomVec2 right_offset = compute_offset(zero, 1.0F, right_center, 2.0F, frame, mode, policy);
  expect_near("4 HStack letterbox no Y leak at left-center", left_offset.y, 0.0F);
  expect_near("4 HStack letterbox no Y leak at seam", seam_offset.y, 0.0F);
  expect_near("4 HStack letterbox no Y leak at right-center", right_offset.y, 0.0F);
}

static void test_vstack_native(const OriginPolicy policy) {
  const Frame frame = kHD;
  const Mode mode = Mode::VStack;
  const ZoomVec2 zero{0.0F, 0.0F};
  const float w = frame.width;
  const float h = frame.height;

  const ZoomVec2 top_center{w * 0.5F, h * 0.5F};
  const ZoomVec2 seam{w * 0.5F, h};
  const ZoomVec2 bottom_center{w * 0.5F, h * 1.5F};
  const ZoomVec2 bottom_tile{w * 0.25F, h * 1.75F};

  expect_focal_point_stable("5 VStack native wheel focal point at top-frame center", top_center, zero, 1.0F, 2.0F, frame, mode, policy);
  expect_focal_point_stable("5 VStack native wheel focal point at seam", seam, zero, 1.0F, 2.0F, frame, mode, policy);
  expect_focal_point_stable("5 VStack native wheel focal point at bottom-frame center", bottom_center, zero, 1.0F, 2.0F, frame, mode, policy);
  expect_focal_point_stable("5 VStack native wheel focal point on bottom tile y>H", bottom_tile, zero, 1.0F, 2.0F, frame, mode, policy);

  const ZoomVec2 top_offset = compute_offset(zero, 1.0F, top_center, 2.0F, frame, mode, policy);
  expect_near("5 VStack canonical top-center offset.x", top_offset.x, 0.0F);
  expect_near("5 VStack canonical top-center offset.y", top_offset.y, 0.0F);

  const ZoomVec2 seam_offset = compute_offset(zero, 1.0F, seam, 2.0F, frame, mode, policy);
  expect_near("5 VStack canonical seam offset.x", seam_offset.x, 0.0F);
  expect_near("5 VStack canonical seam offset.y", seam_offset.y, -540.0F);
}

static void test_vstack_pillarboxed(const OriginPolicy policy) {
  const Frame frame = kHD;
  const Mode mode = Mode::VStack;
  const ZoomVec2 layout = layout_size(mode, frame);
  const ContentWindow content = fit_content_window(1920.0F, 1080.0F, layout.x / layout.y);
  const ZoomVec2 zero{0.0F, 0.0F};
  const float w = frame.width;
  const float h = frame.height;

  if (!(content.x > 0.0F && near(content.y, 0.0F))) {
    std::fprintf(stderr, "FAIL 6 VStack pillarbox fixture: expected side bars, got x=%.1f y=%.1f w=%.1f h=%.1f\n", content.x, content.y, content.w, content.h);
    failures++;
    return;
  }
  std::printf("PASS 6 VStack pillarbox fixture -> content (%.1f, %.1f) %.1fx%.1f\n", content.x, content.y, content.w, content.h);

  const ZoomVec2 top_center{w * 0.5F, h * 0.5F};
  const ZoomVec2 seam{w * 0.5F, h};
  const ZoomVec2 bottom_center{w * 0.5F, h * 1.5F};

  expect_focal_point_stable("6 VStack pillarbox wheel focal point at top-frame center", top_center, zero, 1.0F, 2.0F, frame, mode, policy);
  expect_focal_point_stable("6 VStack pillarbox wheel focal point at seam", seam, zero, 1.0F, 2.0F, frame, mode, policy);
  expect_focal_point_stable("6 VStack pillarbox wheel focal point at bottom-frame center", bottom_center, zero, 1.0F, 2.0F, frame, mode, policy);

  const ZoomVec2 top_offset = compute_offset(zero, 1.0F, top_center, 2.0F, frame, mode, policy);
  const ZoomVec2 seam_offset = compute_offset(zero, 1.0F, seam, 2.0F, frame, mode, policy);
  const ZoomVec2 bottom_offset = compute_offset(zero, 1.0F, bottom_center, 2.0F, frame, mode, policy);
  expect_near("6 VStack pillarbox no X leak at top-center", top_offset.x, 0.0F);
  expect_near("6 VStack pillarbox no X leak at seam", seam_offset.x, 0.0F);
  expect_near("6 VStack pillarbox no X leak at bottom-center", bottom_offset.x, 0.0F);
}

static void test_pan_then_zoom(const OriginPolicy policy) {
  const Frame frame = kHD;
  const ZoomVec2 start_offset{180.0F, -75.0F};
  const float start_zoom = 1.5F;

  expect_focal_point_stable("7 Split pan+zoom off-center focal point", {400.0F, 300.0F}, start_offset, start_zoom, 2.25F, frame, Mode::Split, policy);
  expect_focal_point_stable("7 HStack pan+zoom focal point on right tile x>W", {frame.width * 1.6F, frame.height * 0.35F}, start_offset, start_zoom, 2.25F, frame, Mode::HStack, policy);
  expect_focal_point_stable("7 VStack pan+zoom focal point on bottom tile y>H", {frame.width * 0.35F, frame.height * 1.6F}, start_offset, start_zoom, 2.25F, frame, Mode::VStack, policy);
}

static void test_keyboard_zoom(const OriginPolicy policy) {
  const Frame frame = kHD;
  const ZoomVec2 zero{0.0F, 0.0F};

  // Keys 4-9 keep zoom_point = video_layout_size() * 0.5F (the stack seam).
  auto keyboard_offset = [&](const Mode mode, const float new_zoom) {
    const ZoomVec2 zoom_point{layout_width(mode, frame) * 0.5F, layout_height(mode, frame) * 0.5F};
    return compute_offset(zero, 1.0F, zoom_point, new_zoom, frame, mode, policy);
  };

  const ZoomVec2 split_5 = keyboard_offset(Mode::Split, 0.5F);
  const ZoomVec2 split_7 = keyboard_offset(Mode::Split, 2.0F);
  expect_vec("8 Split key 5 keeps frame-center focal point centered", split_5, zero);
  expect_vec("8 Split key 7 keeps frame-center focal point centered", split_7, zero);

  const ZoomVec2 hstack_7 = keyboard_offset(Mode::HStack, 2.0F);
  expect_near("8 HStack key 7 seam focal point translation x", hstack_7.x, -960.0F);
  expect_near("8 HStack key 7 seam focal point translation y", hstack_7.y, 0.0F);

  const ZoomVec2 hstack_5 = keyboard_offset(Mode::HStack, 0.5F);
  expect_near("8 HStack key 5 seam focal point translation x", hstack_5.x, 480.0F);
  expect_near("8 HStack key 5 seam focal point translation y", hstack_5.y, 0.0F);

  const ZoomVec2 vstack_7 = keyboard_offset(Mode::VStack, 2.0F);
  expect_near("8 VStack key 7 seam focal point translation x", vstack_7.x, 0.0F);
  expect_near("8 VStack key 7 seam focal point translation y", vstack_7.y, -540.0F);

  expect_focal_point_stable("8 Split key 7 focal point is layout center", {frame.width * 0.5F, frame.height * 0.5F}, zero, 1.0F, 2.0F, frame, Mode::Split, policy);
  expect_focal_point_stable("8 HStack key 7 focal point is seam", {frame.width, frame.height * 0.5F}, zero, 1.0F, 2.0F, frame, Mode::HStack, policy);
  expect_focal_point_stable("8 VStack key 7 focal point is seam", {frame.width * 0.5F, frame.height}, zero, 1.0F, 2.0F, frame, Mode::VStack, policy);
}

static void test_zoom_out(const OriginPolicy policy) {
  const Frame frame = kHD;
  const ZoomVec2 zero{0.0F, 0.0F};

  expect_focal_point_stable("9 Split zoom-out off-center focal point", {400.0F, 220.0F}, zero, 1.0F, 0.5F, frame, Mode::Split, policy);
  expect_focal_point_stable("9 HStack zoom-out focal point on right tile", {frame.width * 1.4F, 300.0F}, zero, 2.0F, 1.0F, frame, Mode::HStack, policy);
  expect_focal_point_stable("9 VStack zoom-out focal point on bottom tile", {500.0F, frame.height * 1.4F}, {60.0F, -30.0F}, 2.0F, 0.75F, frame, Mode::VStack, policy);
}

static void test_repeated_zoom(const OriginPolicy policy) {
  const Frame frame = kHD;
  const float zooms[] = {1.0F, 1.25F, 1.5625F, 2.0F, 2.5F};

  auto run_repeat = [&](const char* label, const Mode mode, const ZoomVec2 cursor, ZoomVec2 offset) {
    ZoomRectState rect = zoom_rect_from_move_offset(offset, zooms[0], frame);
    const ZoomVec2 source0 = zoom_transform::zoom_space_to_video_point(cursor, rect, frame.width, frame.height);
    for (size_t i = 1; i < sizeof(zooms) / sizeof(zooms[0]); ++i) {
      offset = compute_offset(offset, zooms[i - 1], cursor, zooms[i], frame, mode, policy);
      rect = zoom_rect_from_move_offset(offset, zooms[i], frame);
      const ZoomVec2 source = zoom_transform::zoom_space_to_video_point(cursor, rect, frame.width, frame.height);
      if (!near_vec(source, source0)) {
        std::fprintf(stderr, "FAIL %s step %zu: source drifted from (%.6f, %.6f) to (%.6f, %.6f)\n", label, i, source0.x, source0.y, source.x, source.y);
        failures++;
        return;
      }
    }
    std::printf("PASS %s\n", label);
  };

  run_repeat("10 Split repeated zoom off-center focal point", Mode::Split, {350.0F, 700.0F}, {0.0F, 0.0F});
  run_repeat("10 HStack repeated zoom focal point on right tile", Mode::HStack, {frame.width * 1.55F, 420.0F}, {0.0F, 0.0F});
  run_repeat("10 VStack repeated zoom focal point on bottom tile", Mode::VStack, {700.0F, frame.height * 1.55F}, {0.0F, 0.0F});
}

// Complete window -> content_window -> layout zoom_point -> offset -> zoom_rect chain.
static void expect_window_chain(const char* label, const Mode mode, const ContentWindow& content, const ZoomVec2 source, const ZoomVec2 move_offset, const float current_zoom, const float new_zoom, const Frame& frame,
                                const OriginPolicy policy) {
  const ZoomVec2 layout = layout_size(mode, frame);
  const ZoomRectState rect_before = zoom_rect_from_move_offset(move_offset, current_zoom, frame);
  const ZoomVec2 mapped_before = zoom_transform::video_point_to_zoom_space(source, rect_before);
  const ZoomVec2 window_mouse = layout_to_window(mapped_before, content, layout);
  const ZoomVec2 zoom_point = window_to_layout(window_mouse, content, layout);

  if (!near_vec(zoom_point, mapped_before)) {
    std::fprintf(stderr, "FAIL %s: window->layout zoom_point (%.6f, %.6f) != mapped (%.6f, %.6f)\n", label, zoom_point.x, zoom_point.y, mapped_before.x, mapped_before.y);
    failures++;
    return;
  }

  const ZoomVec2 leaked{window_mouse.x * (layout.x / content.w), window_mouse.y * (layout.y / content.h)};
  const bool has_content_origin = (content.x > 0.0F) || (content.y > 0.0F);
  if (has_content_origin && near_vec(leaked, zoom_point)) {
    std::fprintf(stderr, "FAIL %s: skipping content_window origin did not change zoom_point\n", label);
    failures++;
    return;
  }

  const ZoomVec2 source_from_mouse = zoom_transform::zoom_space_to_video_point(zoom_point, rect_before, frame.width, frame.height);
  if (!near_vec(source_from_mouse, source)) {
    std::fprintf(stderr, "FAIL %s: zoom_rect inverse of window zoom_point (%.6f, %.6f) != source (%.6f, %.6f)\n", label, source_from_mouse.x, source_from_mouse.y, source.x, source.y);
    failures++;
    return;
  }

  const ZoomVec2 new_offset = compute_offset(move_offset, current_zoom, zoom_point, new_zoom, frame, mode, policy);
  const ZoomRectState rect_after = zoom_rect_from_move_offset(new_offset, new_zoom, frame);
  const ZoomVec2 mapped_after = zoom_transform::video_point_to_zoom_space(source, rect_after);
  if (!near_vec(mapped_after, zoom_point)) {
    std::fprintf(stderr, "FAIL %s: source mapped to (%.6f, %.6f) after zoom, expected focal (%.6f, %.6f)\n", label, mapped_after.x, mapped_after.y, zoom_point.x, zoom_point.y);
    failures++;
    return;
  }
  std::printf("PASS %s\n", label);
}

static void test_window_letterbox_chain(const OriginPolicy policy) {
  const Frame frame = kHD;

  {
    const Mode mode = Mode::HStack;
    const ZoomVec2 layout = layout_size(mode, frame);
    const ContentWindow content = fit_content_window(1920.0F, 1080.0F, layout.x / layout.y);
    if (!(content.y > 0.0F && near(content.x, 0.0F))) {
      std::fprintf(stderr, "FAIL 11 HStack window-chain fixture: expected letterbox, got x=%.1f y=%.1f w=%.1f h=%.1f\n", content.x, content.y, content.w, content.h);
      failures++;
    } else {
      std::printf("PASS 11 HStack window-chain fixture -> content (%.1f, %.1f) %.1fx%.1f\n", content.x, content.y, content.w, content.h);
      expect_window_chain("11 HStack letterbox window chain left-frame", mode, content, {frame.width * 0.5F, frame.height * 0.5F}, {0.0F, 0.0F}, 1.0F, 2.0F, frame, policy);
      expect_window_chain("11 HStack letterbox window chain seam", mode, content, {frame.width, frame.height * 0.5F}, {0.0F, 0.0F}, 1.0F, 2.0F, frame, policy);
      expect_window_chain("11 HStack letterbox window chain right tile x>W with pan+zoom", mode, content, {frame.width * 1.55F, frame.height * 0.3F}, {180.0F, -75.0F}, 1.5F, 2.25F, frame, policy);
    }
  }

  {
    const Mode mode = Mode::VStack;
    const ZoomVec2 layout = layout_size(mode, frame);
    const ContentWindow content = fit_content_window(1920.0F, 1080.0F, layout.x / layout.y);
    if (!(content.x > 0.0F && near(content.y, 0.0F))) {
      std::fprintf(stderr, "FAIL 11 VStack window-chain fixture: expected pillarbox, got x=%.1f y=%.1f w=%.1f h=%.1f\n", content.x, content.y, content.w, content.h);
      failures++;
    } else {
      std::printf("PASS 11 VStack window-chain fixture -> content (%.1f, %.1f) %.1fx%.1f\n", content.x, content.y, content.w, content.h);
      expect_window_chain("11 VStack pillarbox window chain top-frame", mode, content, {frame.width * 0.5F, frame.height * 0.5F}, {0.0F, 0.0F}, 1.0F, 2.0F, frame, policy);
      expect_window_chain("11 VStack pillarbox window chain seam", mode, content, {frame.width * 0.5F, frame.height}, {0.0F, 0.0F}, 1.0F, 2.0F, frame, policy);
      expect_window_chain("11 VStack pillarbox window chain bottom tile y>H with pan+zoom", mode, content, {frame.width * 0.3F, frame.height * 1.55F}, {180.0F, -75.0F}, 1.5F, 2.25F, frame, policy);
    }
  }
}

static void test_transform_origin_is_frame_center() {
  const ZoomVec2 origin = zoom_transform::zoom_transform_origin(kHD.width, kHD.height);
  expect_vec("transform origin is single-frame center", origin, {960.0F, 540.0F});
}

static void run_suite(const OriginPolicy policy) {
  test_transform_origin_is_frame_center();
  test_split_native(policy);
  test_split_letterboxed(policy);
  test_hstack_native(policy);
  test_hstack_letterboxed(policy);
  test_vstack_native(policy);
  test_vstack_pillarboxed(policy);
  test_pan_then_zoom(policy);
  test_keyboard_zoom(policy);
  test_zoom_out(policy);
  test_repeated_zoom(policy);
  test_window_letterbox_chain(policy);
}

int main(int argc, char** argv) {
  const bool pre_fix = argc > 1 && std::strcmp(argv[1], "--pre-fix") == 0;
  const OriginPolicy policy = pre_fix ? OriginPolicy::LayoutCenter : OriginPolicy::FrameCenter;

  std::printf("Running zoom transform tests (%s transform origin)\n", pre_fix ? "pre-fix layout-center" : "production frame-center");
  run_suite(policy);

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }

  std::printf("All zoom transform tests passed\n");
  return EXIT_SUCCESS;
}
