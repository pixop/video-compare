#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

struct CropRect {
  int x{0};
  int y{0};
  int w{0};
  int h{0};
};

struct CropState {
  CropRect rect{};
  bool enabled{false};
};

enum class CropCopyRequest { None, LeftToAllRights, ActiveRightToLeft };

struct PendingCropCopy {
  CropCopyRequest request{CropCopyRequest::None};
  size_t right_target_index{0};
  bool swap_left_right{false};
};

namespace video_filter_state {

struct CropTarget {
  CropState crop;
  std::vector<CropState> history;
  int width{0};
  int height{0};
};

using CropOperation = std::vector<size_t>;

inline bool crop_rects_equal(const CropRect& a, const CropRect& b) {
  return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

inline bool crop_states_equal(const CropState& a, const CropState& b) {
  return a.enabled == b.enabled && (!a.enabled || crop_rects_equal(a.rect, b.rect));
}

inline CropState crop_state_from_rect(const CropRect* rect) {
  CropState state;
  if (rect != nullptr && rect->w > 0 && rect->h > 0) {
    state.rect = *rect;
    state.enabled = true;
  }
  return state;
}

// By-value copy of the existing crop representation. Disabled source crop stays disabled.
inline CropState copied_crop_state(const CropState& source) {
  return source;
}

inline CropState current_crop_from_history(const std::vector<CropState>& history) {
  return history.empty() ? CropState{} : history.back();
}

inline bool record_crop_if_changed(std::vector<CropState>& history, const CropState& current, const CropState& next, CropState& applied) {
  applied = copied_crop_state(next);
  if (crop_states_equal(current, applied)) {
    return false;
  }
  history.push_back(applied);
  return true;
}

constexpr int kMinCropDimension = 2;

// Same rounding as Shift+B stretch mapping in VideoCompare / conversion_geometry.
inline int map_crop_edge(const int edge, const int source_extent, const int dest_extent) {
  if (source_extent == dest_extent) {
    return edge;
  }
  return static_cast<int>(std::llround(static_cast<double>(edge) * dest_extent / source_extent));
}

inline CropState map_crop_state(const CropState& source, const int source_width, const int source_height, const int dest_width, const int dest_height) {
  if (!source.enabled) {
    return {};
  }
  if (source_width <= 0 || source_height <= 0 || dest_width <= 0 || dest_height <= 0) {
    return source;
  }
  if (source_width == dest_width && source_height == dest_height) {
    return source;
  }

  int x0 = map_crop_edge(source.rect.x, source_width, dest_width);
  int y0 = map_crop_edge(source.rect.y, source_height, dest_height);
  int x1 = map_crop_edge(source.rect.x + source.rect.w, source_width, dest_width);
  int y1 = map_crop_edge(source.rect.y + source.rect.h, source_height, dest_height);

  x0 = std::max(0, std::min(x0, dest_width - kMinCropDimension));
  y0 = std::max(0, std::min(y0, dest_height - kMinCropDimension));
  x1 = std::max(x0 + kMinCropDimension, std::min(x1, dest_width));
  y1 = std::max(y0 + kMinCropDimension, std::min(y1, dest_height));

  CropState mapped;
  mapped.enabled = true;
  mapped.rect.x = x0;
  mapped.rect.y = y0;
  mapped.rect.w = x1 - x0;
  mapped.rect.h = y1 - y0;
  return mapped;
}

inline CropState compose_mapped_crop(const CropState& previous, const CropRect& mapped) {
  CropState next;
  next.enabled = true;
  if (previous.enabled) {
    next.rect.x = previous.rect.x + mapped.x;
    next.rect.y = previous.rect.y + mapped.y;
    next.rect.w = mapped.w;
    next.rect.h = mapped.h;
  } else {
    next.rect = mapped;
  }
  return next;
}

inline bool push_crop(CropTarget& target, const CropState& next) {
  CropState applied;
  if (!record_crop_if_changed(target.history, target.crop, next, applied)) {
    return false;
  }
  target.crop = applied;
  return true;
}

inline bool pop_crop(CropTarget& target) {
  if (target.history.empty()) {
    return false;
  }
  target.history.pop_back();
  target.crop = current_crop_from_history(target.history);
  return true;
}

template <typename Target>
bool apply_crop_to_indices(Target* targets, std::vector<CropOperation>& operations, const size_t* indices, const size_t count, const CropState& source) {
  CropOperation changed;
  for (size_t n = 0; n < count; ++n) {
    if (push_crop(targets[indices[n]], source)) {
      changed.push_back(indices[n]);
    }
  }
  if (changed.empty()) {
    return false;
  }
  operations.push_back(changed);
  return true;
}

template <typename Target>
bool undo_last_crop_operation(Target* targets, std::vector<CropOperation>& operations) {
  if (operations.empty()) {
    return false;
  }
  const CropOperation affected = operations.back();
  operations.pop_back();
  bool changed = false;
  for (size_t i : affected) {
    changed = pop_crop(targets[i]) || changed;
  }
  return changed;
}

struct InteractiveCropTargets {
  bool apply_to_left{false};
  bool apply_to_right{false};
  size_t right_index{0};
};

inline InteractiveCropTargets resolve_interactive_crop_targets(const bool apply_visual_left, const bool apply_visual_right, const bool swap_left_right, const size_t snapshotted_right_index) {
  InteractiveCropTargets targets;
  targets.right_index = snapshotted_right_index;
  if (swap_left_right) {
    targets.apply_to_left = apply_visual_right;
    targets.apply_to_right = apply_visual_left;
  } else {
    targets.apply_to_left = apply_visual_left;
    targets.apply_to_right = apply_visual_right;
  }
  return targets;
}

struct SelectedRight {
  bool valid{false};
  size_t index{0};
};

inline SelectedRight selected_right(const size_t active_index, const size_t right_count) {
  if (right_count == 0) {
    return {};
  }
  SelectedRight selected;
  selected.valid = true;
  selected.index = active_index < right_count ? active_index : (right_count - 1);
  return selected;
}

// Visual left/right at operation time, matching Shift+L / Shift+R after Swap.
struct CropCopyPlan {
  bool valid{false};
  bool source_is_left{false};
  size_t source_right_index{0};
  bool copy_to_all_rights{false};
  bool dest_is_left{false};
  size_t dest_right_index{0};
};

inline CropCopyPlan resolve_crop_copy(const CropCopyRequest request, const bool swap_left_right, const size_t snapshotted_right_index, const size_t right_count) {
  CropCopyPlan plan;
  const SelectedRight selected = selected_right(snapshotted_right_index, right_count);

  if (request == CropCopyRequest::LeftToAllRights) {
    if (swap_left_right) {
      if (!selected.valid) {
        return {};
      }
      plan.source_is_left = false;
      plan.source_right_index = selected.index;
    } else {
      plan.source_is_left = true;
    }
    plan.valid = true;
    plan.copy_to_all_rights = true;
    // After Swap the visual right is the logical left; include it so the on-screen pair updates.
    plan.dest_is_left = swap_left_right;
    return plan;
  }

  if (request == CropCopyRequest::ActiveRightToLeft) {
    if (!selected.valid) {
      return {};
    }
    plan.valid = true;
    if (swap_left_right) {
      plan.source_is_left = true;
      plan.dest_is_left = false;
      plan.dest_right_index = selected.index;
    } else {
      plan.source_is_left = false;
      plan.source_right_index = selected.index;
      plan.dest_is_left = true;
    }
    return plan;
  }

  return {};
}

template <typename Target>
bool copy_crop(Target* targets,
               std::vector<CropOperation>& operations,
               const CropCopyRequest request,
               const bool swap_left_right,
               const size_t left_index,
               const size_t first_right_index,
               const size_t right_count,
               const size_t snapshotted_right_index) {
  const CropCopyPlan plan = resolve_crop_copy(request, swap_left_right, snapshotted_right_index, right_count);
  if (!plan.valid || targets == nullptr) {
    return false;
  }

  const size_t source_index = plan.source_is_left ? left_index : (first_right_index + plan.source_right_index);
  std::vector<size_t> destinations;
  if (plan.dest_is_left) {
    destinations.push_back(left_index);
  }
  if (plan.copy_to_all_rights) {
    for (size_t i = 0; i < right_count; ++i) {
      destinations.push_back(first_right_index + i);
    }
  } else if (!plan.dest_is_left) {
    destinations.push_back(first_right_index + plan.dest_right_index);
  }

  CropOperation changed;
  const CropState source_crop = targets[source_index].crop;
  for (const size_t dest_index : destinations) {
    const CropState mapped = map_crop_state(source_crop, targets[source_index].width, targets[source_index].height, targets[dest_index].width, targets[dest_index].height);
    if (push_crop(targets[dest_index], mapped)) {
      changed.push_back(dest_index);
    }
  }
  if (changed.empty()) {
    return false;
  }
  operations.push_back(changed);
  return true;
}

inline std::string crop_filter(const CropRect& rect) {
  return "crop=" + std::to_string(rect.w) + ":" + std::to_string(rect.h) + ":" + std::to_string(rect.x) + ":" + std::to_string(rect.y);
}

// Top-level comma-separated filter instances in a linear graph fragment.
// Quotes and backslash escapes are respected so option values may contain commas.
inline int count_linear_filter_instances(const std::string& description) {
  if (description.empty()) {
    return 0;
  }

  int count = 1;
  bool escaped = false;
  bool in_single_quote = false;
  for (const char ch : description) {
    if (escaped) {
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '\'') {
      in_single_quote = !in_single_quote;
      continue;
    }
    if (ch == ',' && !in_single_quote) {
      ++count;
    }
  }
  return count;
}

inline std::string compose_filters(const std::string& pre_filters, const std::string& post_filters, const CropRect& rect, const bool crop_enabled) {
  std::string result;
  auto append_group = [&](const std::string& group) {
    if (group.empty()) {
      return;
    }
    if (!result.empty()) {
      result += ',';
    }
    result += group;
  };

  append_group(pre_filters);
  if (crop_enabled) {
    append_group(crop_filter(rect));
  }
  append_group(post_filters);

  return result.empty() ? "copy" : result;
}

inline std::string filters_for_display_state(const std::string& filters) {
  return filters.empty() ? "copy" : filters;
}

inline std::string quote_display_state_value(const std::string& value) {
  std::string quoted;
  quoted.push_back('"');
  for (const char ch : value) {
    if (ch == '"' || ch == '\\') {
      quoted.push_back('\\');
    }
    quoted.push_back(ch);
  }
  quoted.push_back('"');
  return quoted;
}

inline size_t display_state_right_id(const size_t zero_based_right_index) {
  return zero_based_right_index + 1;
}

inline void append_display_state_mapping(std::string& message, const bool swapped, const size_t zero_based_right_index) {
  message += swapped ? " swapped=true" : " swapped=false";
  message += " right=" + std::to_string(display_state_right_id(zero_based_right_index));
}

inline void append_display_state_filters(std::string& message, const std::string& left_filters, const std::string& right_filters) {
  message += " filters_left=" + quote_display_state_value(filters_for_display_state(left_filters));
  message += " filters_right=" + quote_display_state_value(filters_for_display_state(right_filters));
}

}  // namespace video_filter_state
