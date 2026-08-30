#pragma once

// Production zoom math shared by Display and its unit tests.
//
// Terminology:
//   transform origin / pivot: always the single-frame center (W/2, H/2).
//     global_center_ and compute_zoom_rect() normalize with video_width_ /
//     video_height_, not the stacked layout size.
//   zoom focal point (zoom_point): a content-relative coordinate in combined
//     layout space (Split: WxH, HStack: 2WxH, VStack: Wx2H). It may be the
//     cursor or, for keys 4-9, video_layout_size()*0.5 (the stack seam).
//
// HStack/VStack use the single-frame center as the zoom transform origin
// while preserving layout-space focal points such as the cursor or stack seam.
// Do not include content_window_.x/y in the origin; zoom_point is already
// content-relative after subtracting the content_window origin.

namespace zoom_transform {

struct ZoomVec2 {
  float x;
  float y;
};

struct ZoomRectState {
  ZoomVec2 start;
  ZoomVec2 end;
  ZoomVec2 size;
  float zoom_factor;
};

inline ZoomVec2 zoom_transform_origin(const float frame_width, const float frame_height) {
  return {frame_width * 0.5F, frame_height * 0.5F};
}

inline ZoomVec2 compute_zoom_move_offset(const ZoomVec2 move_offset, const float current_zoom_factor, const ZoomVec2 zoom_point, const float new_zoom_factor, const float frame_width, const float frame_height) {
  const float zoom_factor_change = new_zoom_factor / current_zoom_factor;
  const ZoomVec2 transform_origin = zoom_transform_origin(frame_width, frame_height);
  return {move_offset.x - (transform_origin.x + move_offset.x - zoom_point.x) * (1.0F - zoom_factor_change), move_offset.y - (transform_origin.y + move_offset.y - zoom_point.y) * (1.0F - zoom_factor_change)};
}

// Display::update_move_offset()
inline ZoomVec2 zoom_global_center_from_move_offset(const ZoomVec2 move_offset, const float frame_width, const float frame_height) {
  return {move_offset.x / frame_width + 0.5F, move_offset.y / frame_height + 0.5F};
}

// Display::compute_zoom_rect()
inline ZoomRectState compute_zoom_rect_state(const ZoomVec2 global_center, const float zoom_factor, const float frame_width, const float frame_height) {
  const ZoomVec2 video_extent{frame_width, frame_height};
  const ZoomVec2 start{(global_center.x - zoom_factor * 0.5F) * video_extent.x, (global_center.y - zoom_factor * 0.5F) * video_extent.y};
  const ZoomVec2 end{(global_center.x + zoom_factor * 0.5F) * video_extent.x, (global_center.y + zoom_factor * 0.5F) * video_extent.y};
  return {start, end, {end.x - start.x, end.y - start.y}, zoom_factor};
}

// Display::video_to_zoom_space() position (width/height clipping stays in Display)
inline ZoomVec2 video_point_to_zoom_space(const ZoomVec2 layout_point, const ZoomRectState& zoom_rect) {
  return {zoom_rect.start.x + layout_point.x * zoom_rect.zoom_factor, zoom_rect.start.y + layout_point.y * zoom_rect.zoom_factor};
}

// Display::window_to_video_position() after the content-window -> layout conversion,
// before integer floor/ceil.
inline ZoomVec2 zoom_space_to_video_point(const ZoomVec2 mapped_point, const ZoomRectState& zoom_rect, const float frame_width, const float frame_height) {
  return {(mapped_point.x - zoom_rect.start.x) * frame_width / zoom_rect.size.x, (mapped_point.y - zoom_rect.start.y) * frame_height / zoom_rect.size.y};
}

}  // namespace zoom_transform
