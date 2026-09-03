#include "video_filter_state.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

static int failures = 0;

static void expect_bool(const char* label, const bool got, const bool expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: expected %d, got %d\n", label, static_cast<int>(expected), static_cast<int>(got));
    failures++;
    return;
  }
  std::printf("PASS %s -> %s\n", label, got ? "true" : "false");
}

static void expect_size(const char* label, const size_t got, const size_t expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: expected %zu, got %zu\n", label, expected, got);
    failures++;
    return;
  }
  std::printf("PASS %s -> %zu\n", label, got);
}

static void expect_str(const char* label, const std::string& got, const std::string& expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: expected '%s', got '%s'\n", label, expected.c_str(), got.c_str());
    failures++;
    return;
  }
  std::printf("PASS %s -> %s\n", label, got.c_str());
}

static void expect_operation(const char* label, const video_filter_state::CropOperation& got, const std::initializer_list<size_t> expected) {
  const std::vector<size_t> expected_sides(expected);
  if (got != expected_sides) {
    std::fprintf(stderr, "FAIL %s: operation sides mismatch\n", label);
    failures++;
    return;
  }
  std::printf("PASS %s -> %zu side(s)\n", label, got.size());
}

static void expect_crop(const char* label, const CropState& got, const CropState& expected) {
  if (!video_filter_state::crop_states_equal(got, expected)) {
    std::fprintf(stderr, "FAIL %s: expected enabled=%d %dx%d+%d+%d, got enabled=%d %dx%d+%d+%d\n", label, static_cast<int>(expected.enabled), expected.rect.w, expected.rect.h, expected.rect.x, expected.rect.y,
                 static_cast<int>(got.enabled), got.rect.w, got.rect.h, got.rect.x, got.rect.y);
    failures++;
    return;
  }
  std::printf("PASS %s\n", label);
}

int main() {
  const CropState no_crop{};
  const CropState left_crop{{10, 20, 300, 200}, true};
  const CropState right_a{{1, 2, 8, 9}, true};
  const CropState right_b{{40, 50, 120, 80}, true};
  const CropState right_c{{7, 8, 16, 16}, true};

  constexpr size_t kLeft = 0;
  constexpr size_t kRight0 = 1;
  constexpr size_t kRight1 = 2;
  constexpr size_t kRight2 = 3;

  video_filter_state::CropTarget sides[4];
  std::vector<video_filter_state::CropOperation> operations;

  const size_t left_only[] = {kLeft};
  expect_bool("crop left-only", video_filter_state::apply_crop_to_indices(sides, operations, left_only, 1, left_crop), true);
  expect_crop("left has crop", sides[kLeft].crop, left_crop);
  expect_crop("right 0 unchanged after left crop", sides[kRight0].crop, no_crop);
  expect_bool("undo left-only", video_filter_state::undo_last_crop_operation(sides, operations), true);
  expect_crop("undo left-only restores no crop", sides[kLeft].crop, no_crop);
  expect_size("undo left-only leaves no operations", operations.size(), 0);

  expect_bool("crop left again", video_filter_state::apply_crop_to_indices(sides, operations, left_only, 1, left_crop), true);
  const size_t right0_only[] = {kRight0};
  const size_t right1_only[] = {kRight1};
  expect_bool("crop other right first", video_filter_state::apply_crop_to_indices(sides, operations, right1_only, 1, right_b), true);
  expect_bool("crop current right only", video_filter_state::apply_crop_to_indices(sides, operations, right0_only, 1, right_a), true);
  expect_bool("undo current-right-only", video_filter_state::undo_last_crop_operation(sides, operations), true);
  expect_crop("current right restored without touching other right", sides[kRight0].crop, no_crop);
  expect_crop("other right kept after undoing current right", sides[kRight1].crop, right_b);
  expect_crop("left kept after undoing current right", sides[kLeft].crop, left_crop);

  const size_t both[] = {kLeft, kRight0};
  const CropState both_crop{{5, 6, 50, 40}, true};
  expect_bool("crop both", video_filter_state::apply_crop_to_indices(sides, operations, both, 2, both_crop), true);
  expect_crop("both left", sides[kLeft].crop, both_crop);
  expect_crop("both current right", sides[kRight0].crop, both_crop);
  expect_bool("undo both", video_filter_state::undo_last_crop_operation(sides, operations), true);
  expect_crop("undo both restores left", sides[kLeft].crop, left_crop);
  expect_crop("undo both restores current right", sides[kRight0].crop, no_crop);
  expect_crop("undo both leaves other right untouched", sides[kRight1].crop, right_b);

  expect_bool("seed right 0 as A", video_filter_state::apply_crop_to_indices(sides, operations, right0_only, 1, right_a), true);
  expect_crop("right 2 starts with no crop", sides[kRight2].crop, no_crop);
  const size_t all_rights[] = {kRight0, kRight1, kRight2};
  expect_bool("copy left to all rights", video_filter_state::copy_crop(sides, operations, CropCopyRequest::LeftToAllRights, false, kLeft, kRight0, 3, 0), true);
  expect_crop("right 0 received left copy", sides[kRight0].crop, left_crop);
  expect_crop("right 1 received left copy", sides[kRight1].crop, left_crop);
  expect_crop("right 2 received left copy", sides[kRight2].crop, left_crop);
  expect_bool("undo left-to-all-rights copy", video_filter_state::undo_last_crop_operation(sides, operations), true);
  expect_crop("undo copy restores right 0 to A", sides[kRight0].crop, right_a);
  expect_crop("undo copy restores right 1 to B", sides[kRight1].crop, right_b);
  expect_crop("undo copy restores right 2 to no crop", sides[kRight2].crop, no_crop);
  expect_crop("undo copy leaves left unchanged", sides[kLeft].crop, left_crop);

  expect_bool("copy current right to left", video_filter_state::copy_crop(sides, operations, CropCopyRequest::ActiveRightToLeft, false, kLeft, kRight0, 3, 0), true);
  expect_crop("left received current right crop", sides[kLeft].crop, right_a);
  expect_crop("current right unchanged by copy to left", sides[kRight0].crop, right_a);
  expect_bool("undo current-right-to-left", video_filter_state::undo_last_crop_operation(sides, operations), true);
  expect_crop("undo copy to left restores previous left", sides[kLeft].crop, left_crop);

  video_filter_state::CropTarget sequence[4];
  std::vector<video_filter_state::CropOperation> sequence_ops;
  expect_bool("seq crop left", video_filter_state::apply_crop_to_indices(sequence, sequence_ops, left_only, 1, left_crop), true);
  expect_bool("seq crop current right", video_filter_state::apply_crop_to_indices(sequence, sequence_ops, right0_only, 1, right_a), true);
  expect_bool("seq crop other right", video_filter_state::apply_crop_to_indices(sequence, sequence_ops, right1_only, 1, right_b), true);
  expect_bool("seq copy left to all rights", video_filter_state::copy_crop(sequence, sequence_ops, CropCopyRequest::LeftToAllRights, false, kLeft, kRight0, 3, 0), true);
  expect_size("seq has 4 operations", sequence_ops.size(), 4);
  expect_bool("seq undo copy", video_filter_state::undo_last_crop_operation(sequence, sequence_ops), true);
  expect_crop("seq after first undo right 0 is A", sequence[kRight0].crop, right_a);
  expect_crop("seq after first undo right 1 is B", sequence[kRight1].crop, right_b);
  expect_crop("seq after first undo right 2 is none", sequence[kRight2].crop, no_crop);
  expect_crop("seq after first undo left stays", sequence[kLeft].crop, left_crop);
  expect_bool("seq undo other right", video_filter_state::undo_last_crop_operation(sequence, sequence_ops), true);
  expect_crop("seq after second undo other right cleared", sequence[kRight1].crop, no_crop);
  expect_crop("seq after second undo current right kept", sequence[kRight0].crop, right_a);
  expect_bool("seq undo current right", video_filter_state::undo_last_crop_operation(sequence, sequence_ops), true);
  expect_crop("seq after third undo current right cleared", sequence[kRight0].crop, no_crop);
  expect_crop("seq after third undo left kept", sequence[kLeft].crop, left_crop);
  expect_bool("seq undo left", video_filter_state::undo_last_crop_operation(sequence, sequence_ops), true);
  expect_crop("seq after fourth undo left cleared", sequence[kLeft].crop, no_crop);
  expect_size("seq fully unwound", sequence_ops.size(), 0);

  video_filter_state::CropTarget empty_targets[1];
  std::vector<video_filter_state::CropOperation> empty_ops;
  expect_bool("backspace with no crop history is a no-op", video_filter_state::undo_last_crop_operation(empty_targets, empty_ops), false);
  expect_crop("no-op undo leaves crop disabled", empty_targets[0].crop, no_crop);

  CropState source = left_crop;
  video_filter_state::CropTarget dest;
  expect_bool("push copies by value", video_filter_state::push_crop(dest, source), true);
  source.rect.x = 99;
  source.enabled = false;
  expect_crop("dest crop stays independent after source mutation", dest.crop, left_crop);
  expect_size("dest history kept previous state", dest.history.size(), 1);

  expect_bool("pushing the same crop is a no-op", video_filter_state::push_crop(dest, left_crop), false);
  expect_bool("pushing disabled crop is a history step", video_filter_state::push_crop(dest, no_crop), true);
  expect_crop("disabled crop is current after push", dest.crop, no_crop);
  expect_bool("undo disabled copy restores previous crop", video_filter_state::pop_crop(dest), true);
  expect_crop("restored after disabled copy", dest.crop, left_crop);

  const CropState stacked = video_filter_state::compose_mapped_crop(left_crop, {4, 5, 20, 10});
  expect_crop("stacked interactive crop is previous plus mapped", stacked, CropState{{14, 25, 20, 10}, true});
  expect_crop("first interactive crop uses mapped rect", video_filter_state::compose_mapped_crop(no_crop, {10, 20, 300, 200}), left_crop);

  const CropRect display_hd_sel{480, 270, 960, 540};
  expect_crop("display map identity dest==crop-space", video_filter_state::map_display_rect_to_crop_space(display_hd_sel, 1920, 1080, no_crop, 1920, 1080), CropState{{480, 270, 960, 540}, true});
  expect_crop("display map post-scale 25-75 percent", video_filter_state::map_display_rect_to_crop_space({320, 180, 640, 360}, 1280, 720, no_crop, 1920, 1080), CropState{{480, 270, 960, 540}, true});
  const CropState existing_extent{{200, 100, 1000, 500}, true};
  expect_crop("display map existing crop plus post-scale middle 50", video_filter_state::map_display_rect_to_crop_space({320, 180, 640, 360}, 1280, 720, existing_extent, 1920, 1080), CropState{{450, 225, 500, 250}, true});
  expect_crop("display map identity dest==current extent", video_filter_state::map_display_rect_to_crop_space({40, 20, 200, 100}, 1000, 500, existing_extent, 1920, 1080), CropState{{240, 120, 200, 100}, true});
  expect_crop("display map clips to current extent", video_filter_state::map_display_rect_to_crop_space({0, 0, 1280, 720}, 1280, 720, existing_extent, 1920, 1080), existing_extent);
  expect_crop("display map no-current-crop uses full crop-space", video_filter_state::map_display_rect_to_crop_space({0, 0, 1280, 720}, 1280, 720, no_crop, 1920, 1080), CropState{{0, 0, 1920, 1080}, true});
  const int round_x0 = video_filter_state::map_crop_edge(1, 1280, 1920);
  const int round_x1 = video_filter_state::map_crop_edge(11, 1280, 1920);
  const int round_y0 = video_filter_state::map_crop_edge(1, 720, 1080);
  const int round_y1 = video_filter_state::map_crop_edge(11, 720, 1080);
  expect_crop("display map rounds edges with llround", video_filter_state::map_display_rect_to_crop_space({1, 1, 10, 10}, 1280, 720, no_crop, 1920, 1080), CropState{{round_x0, round_y0, round_x1 - round_x0, round_y1 - round_y0}, true});
  expect_crop("display map anisotropic dest", video_filter_state::map_display_rect_to_crop_space({100, 50, 1600, 700}, 1920, 800, no_crop, 1920, 1080),
             CropState{{100, video_filter_state::map_crop_edge(50, 800, 1080), 1600, video_filter_state::map_crop_edge(750, 800, 1080) - video_filter_state::map_crop_edge(50, 800, 1080)}, true});
  const CropState tiny = video_filter_state::map_display_rect_to_crop_space({0, 0, 1, 1}, 1280, 720, no_crop, 1920, 1080);
  expect_bool("display map keeps min 2x2", tiny.rect.w >= 2 && tiny.rect.h >= 2, true);
  const CropState overflow = video_filter_state::map_display_rect_to_crop_space({0, 0, 1280, 720}, 1280, 720, {{1800, 1000, 200, 100}, true}, 1920, 1080);
  expect_bool("display map stays inside crop-space x", overflow.rect.x >= 0 && overflow.rect.x + overflow.rect.w <= 1920, true);
  expect_bool("display map stays inside crop-space y", overflow.rect.y >= 0 && overflow.rect.y + overflow.rect.h <= 1080, true);
  expect_crop("display map pre-transpose crop-space is not src-clamped", video_filter_state::map_display_rect_to_crop_space({0, 0, 1080, 1920}, 1080, 1920, no_crop, 1080, 1920), CropState{{0, 0, 1080, 1920}, true});

  const CropRect both_canvas_on_left{20, 20, 120, 80};
  const CropRect both_canvas_on_right{40, 40, 240, 160};
  expect_crop("Shift+B left maps independently", video_filter_state::map_display_rect_to_crop_space(both_canvas_on_left, 320, 180, no_crop, 320, 180), CropState{{20, 20, 120, 80}, true});
  expect_crop("Shift+B right maps independently", video_filter_state::map_display_rect_to_crop_space(both_canvas_on_right, 640, 360, no_crop, 640, 360), CropState{{40, 40, 240, 160}, true});
  expect_crop("Shift+B right with existing crop uses that extent", video_filter_state::map_display_rect_to_crop_space({0, 0, 320, 180}, 320, 180, {{10, 10, 300, 160}, true}, 640, 360), CropState{{10, 10, 300, 160}, true});

  video_filter_state::CropTarget left_before_empty_rights[1];
  left_before_empty_rights[0].crop = left_crop;
  left_before_empty_rights[0].history = {left_crop};
  std::vector<video_filter_state::CropOperation> unused_ops;
  expect_bool("right-to-left with no rights is a no-op", video_filter_state::copy_crop(left_before_empty_rights, unused_ops, CropCopyRequest::ActiveRightToLeft, false, 0, 1, 0, 0), false);
  expect_crop("left unchanged when no rights exist", left_before_empty_rights[0].crop, left_crop);
  expect_size("no operation recorded when no rights exist", unused_ops.size(), 0);

  video_filter_state::CropTarget identity[4];
  std::vector<video_filter_state::CropOperation> identity_ops;
  size_t selected_right = 0;
  expect_bool("crop Right 1 while selected", video_filter_state::apply_crop_to_indices(identity, identity_ops, right1_only, 1, right_a), true);
  expect_operation("recorded Right 1 identity, not current-right", identity_ops.back(), {kRight1});
  selected_right = 1;
  expect_size("selection switch does not rewrite undo identity", identity_ops.back()[0], kRight1);
  expect_bool("undo after selecting Right 2", video_filter_state::undo_last_crop_operation(identity, identity_ops), true);
  expect_crop("Right 1 undone after selection switch", identity[kRight1].crop, no_crop);
  expect_crop("Right 2 untouched after undoing Right 1", identity[kRight2].crop, no_crop);
  expect_crop("left untouched after undoing Right 1", identity[kLeft].crop, no_crop);

  const size_t left_and_right1[] = {kLeft, kRight1};
  expect_bool("crop both left + Right 1", video_filter_state::apply_crop_to_indices(identity, identity_ops, left_and_right1, 2, both_crop), true);
  expect_operation("both records left + Right 1", identity_ops.back(), {kLeft, kRight1});
  selected_right = 1;
  expect_bool("undo both after selecting Right 2", video_filter_state::undo_last_crop_operation(identity, identity_ops), true);
  expect_crop("both-undo restores left after selection switch", identity[kLeft].crop, no_crop);
  expect_crop("both-undo restores Right 1 after selection switch", identity[kRight1].crop, no_crop);
  expect_crop("Right 2 still untouched after both-undo", identity[kRight2].crop, no_crop);
  (void)selected_right;

  expect_bool("seed Right 1 before Shift+I", video_filter_state::apply_crop_to_indices(identity, identity_ops, right1_only, 1, right_a), true);
  expect_bool("Shift+I from Right 1 to left", video_filter_state::copy_crop(identity, identity_ops, CropCopyRequest::ActiveRightToLeft, false, kLeft, kRight0, 3, 1), true);
  expect_operation("Shift+I records left only", identity_ops.back(), {kLeft});
  expect_bool("undo Shift+I after selecting Right 2", video_filter_state::undo_last_crop_operation(identity, identity_ops), true);
  expect_crop("Shift+I undo restores left", identity[kLeft].crop, no_crop);
  expect_crop("Shift+I undo leaves Right 1 source crop", identity[kRight1].crop, right_a);
  expect_crop("Shift+I undo leaves Right 2 untouched", identity[kRight2].crop, no_crop);

  video_filter_state::CropTarget nops[4];
  std::vector<video_filter_state::CropOperation> nop_ops;
  expect_bool("real left crop before no-ops", video_filter_state::apply_crop_to_indices(nops, nop_ops, left_only, 1, left_crop), true);
  expect_bool("interactive crop to same left state is a no-op", video_filter_state::apply_crop_to_indices(nops, nop_ops, left_only, 1, left_crop), false);
  expect_size("same-state crop does not consume an undo step", nop_ops.size(), 1);
  const size_t all_three_rights[] = {kRight0, kRight1, kRight2};
  expect_bool("prime all rights with left crop", video_filter_state::apply_crop_to_indices(nops, nop_ops, all_three_rights, 3, left_crop), true);
  expect_bool("Shift+O when every right already matches left is a no-op", video_filter_state::copy_crop(nops, nop_ops, CropCopyRequest::LeftToAllRights, false, kLeft, kRight0, 3, 0), false);
  expect_size("identical Shift+O does not consume an undo step", nop_ops.size(), 2);
  expect_bool("Shift+I when left already matches current right is a no-op", video_filter_state::copy_crop(nops, nop_ops, CropCopyRequest::ActiveRightToLeft, false, kLeft, kRight0, 3, 0), false);
  expect_size("identical Shift+I does not consume an undo step", nop_ops.size(), 2);
  expect_bool("undo after no-ops restores the last real change", video_filter_state::undo_last_crop_operation(nops, nop_ops), true);
  expect_crop("no-op undo skipped back to primed rights", nops[kRight0].crop, no_crop);
  expect_crop("left still has the earlier real crop", nops[kLeft].crop, left_crop);

  video_filter_state::CropTarget partial[4];
  std::vector<video_filter_state::CropOperation> partial_ops;
  expect_bool("partial: left crop", video_filter_state::apply_crop_to_indices(partial, partial_ops, left_only, 1, left_crop), true);
  expect_bool("partial: Right 0 already matches left", video_filter_state::apply_crop_to_indices(partial, partial_ops, right0_only, 1, left_crop), true);
  expect_bool("partial: Right 1 has B", video_filter_state::apply_crop_to_indices(partial, partial_ops, right1_only, 1, right_b), true);
  expect_bool("partial Shift+O changes only mismatched rights", video_filter_state::copy_crop(partial, partial_ops, CropCopyRequest::LeftToAllRights, false, kLeft, kRight0, 3, 0), true);
  expect_operation("partial Shift+O records only Right 1 and Right 2", partial_ops.back(), {kRight1, kRight2});
  expect_crop("partial Shift+O leaves already-matching Right 0", partial[kRight0].crop, left_crop);
  expect_bool("undo partial Shift+O", video_filter_state::undo_last_crop_operation(partial, partial_ops), true);
  expect_crop("partial undo keeps matching Right 0", partial[kRight0].crop, left_crop);
  expect_crop("partial undo restores Right 1 to B", partial[kRight1].crop, right_b);
  expect_crop("partial undo restores Right 2 to no crop", partial[kRight2].crop, no_crop);

  expect_str("no filters serialize as copy", video_filter_state::compose_filters("", "", {}, false), "copy");
  expect_str("empty dump uses copy", video_filter_state::filters_for_display_state(""), "copy");
  expect_str("crop-only chain", video_filter_state::compose_filters("", "", {10, 20, 300, 200}, true), "crop=300:200:10:20");
  expect_str("pre crop post chain", video_filter_state::compose_filters("setparams=colorspace=bt709", "format=gray", {0, 0, 100, 50}, true), "setparams=colorspace=bt709,crop=100:50:0:0,format=gray");
  expect_str("existing copy is kept", video_filter_state::filters_for_display_state("copy"), "copy");

  expect_size("empty filter string has no instances", static_cast<size_t>(video_filter_state::count_linear_filter_instances("")), 0);
  expect_size("single filter instance", static_cast<size_t>(video_filter_state::count_linear_filter_instances("setparams=colorspace=bt709")), 1);
  expect_size("comma-separated pre-filters", static_cast<size_t>(video_filter_state::count_linear_filter_instances("scale=iw*sar:ih,fps=30.000,transpose=clock")), 3);
  expect_size("quoted comma stays one instance", static_cast<size_t>(video_filter_state::count_linear_filter_instances("geq=lum='X,Y'")), 1);

  expect_str("quote plain filters", video_filter_state::quote_display_state_value("copy"), "\"copy\"");
  expect_str("quote escapes quotes", video_filter_state::quote_display_state_value("a\"b"), "\"a\\\"b\"");
  expect_str("quote escapes backslash", video_filter_state::quote_display_state_value("a\\b"), "\"a\\\\b\"");

  std::string none_line = "Display state: window=800x640";
  video_filter_state::append_display_state_filters(none_line, "", "");
  expect_str("display state no filters", none_line, "Display state: window=800x640 filters_left=\"copy\" filters_right=\"copy\"");

  std::string crop_line = "Display state: window=800x640";
  video_filter_state::append_display_state_filters(crop_line, "crop=300:200:10:20", "setparams=colorspace=bt709,crop=100:50:0:0");
  expect_str("display state with filters", crop_line, "Display state: window=800x640 filters_left=\"crop=300:200:10:20\" filters_right=\"setparams=colorspace=bt709,crop=100:50:0:0\"");

  expect_size("right-video ID is user-facing 1-based", video_filter_state::display_state_right_id(1), 2);

  std::string unswapped = "Display state: window=800x450 aspect=stretch";
  video_filter_state::append_display_state_mapping(unswapped, false, 1);
  video_filter_state::append_display_state_filters(unswapped, "crop=443:327:190:185", "crop=175:152:299:263");
  expect_str("display state unswapped keeps logical filters", unswapped,
             "Display state: window=800x450 aspect=stretch swapped=false right=2 filters_left=\"crop=443:327:190:185\" filters_right=\"crop=175:152:299:263\"");

  std::string swapped = "Display state: window=800x450 aspect=stretch";
  video_filter_state::append_display_state_mapping(swapped, true, 1);
  video_filter_state::append_display_state_filters(swapped, "crop=443:327:190:185", "crop=175:152:299:263");
  expect_str("display state swapped does not exchange logical filters", swapped,
             "Display state: window=800x450 aspect=stretch swapped=true right=2 filters_left=\"crop=443:327:190:185\" filters_right=\"crop=175:152:299:263\"");

  std::string selected = "Display state: window=800x450 aspect=stretch";
  video_filter_state::append_display_state_mapping(selected, false, 2);
  expect_str("display state right ID follows selection", selected, "Display state: window=800x450 aspect=stretch swapped=false right=3");

  std::string swap_keeps_right = "Display state: window=800x450 aspect=stretch";
  video_filter_state::append_display_state_mapping(swap_keeps_right, true, 2);
  expect_str("display state swap does not change right ID", swap_keeps_right, "Display state: window=800x450 aspect=stretch swapped=true right=3");

  const CropState hd_crop{{100, 50, 1600, 900}, true};
  expect_crop("same-size map is identical", video_filter_state::map_crop_state(hd_crop, 1920, 1080, 1920, 1080), hd_crop);
  expect_crop("exact 2x map scales edges", video_filter_state::map_crop_state(hd_crop, 1920, 1080, 3840, 2160), CropState{{200, 100, 3200, 1800}, true});
  expect_crop("exact 1/2 map scales edges", video_filter_state::map_crop_state(CropState{{200, 100, 3200, 1800}, true}, 3840, 2160, 1920, 1080), hd_crop);
  const CropState mapped_720 = video_filter_state::map_crop_state(hd_crop, 1920, 1080, 1280, 720);
  expect_crop("non-integer 2/3 map uses edges", mapped_720, CropState{{67, 33, 1066, 600}, true});
  expect_bool("2/3 mapped x0 is round(100*1280/1920)", mapped_720.rect.x == video_filter_state::map_crop_edge(100, 1920, 1280), true);
  expect_bool("2/3 mapped x1 is round(1700*1280/1920)", mapped_720.rect.x + mapped_720.rect.w == video_filter_state::map_crop_edge(1700, 1920, 1280), true);
  const CropState mapped_wide = video_filter_state::map_crop_state(hd_crop, 1920, 1080, 1920, 800);
  expect_crop("different aspect preserves normalized y edges", mapped_wide, CropState{{100, 37, 1600, 667}, true});
  expect_crop("disabled source maps to disabled dest", video_filter_state::map_crop_state(no_crop, 1920, 1080, 3840, 2160), no_crop);
  const CropState overflowing = video_filter_state::map_crop_state({{10, 10, 3000, 2000}, true}, 1920, 1080, 640, 360);
  expect_bool("mapped crop stays inside dest x", overflowing.rect.x >= 0 && overflowing.rect.x + overflowing.rect.w <= 640, true);
  expect_bool("mapped crop stays inside dest y", overflowing.rect.y >= 0 && overflowing.rect.y + overflowing.rect.h <= 360, true);
  expect_bool("mapped crop keeps min size", overflowing.rect.w >= 2 && overflowing.rect.h >= 2, true);

  video_filter_state::CropTarget sized[4];
  sized[kLeft].width = 1920;
  sized[kLeft].height = 1080;
  sized[kRight0].width = 3840;
  sized[kRight0].height = 2160;
  sized[kRight1].width = 1920;
  sized[kRight1].height = 1080;
  sized[kRight2].width = 1280;
  sized[kRight2].height = 720;
  sized[kLeft].crop = hd_crop;
  sized[kLeft].history = {hd_crop};
  sized[kRight0].crop = right_a;
  sized[kRight0].history = {right_a};
  sized[kRight1].crop = right_b;
  sized[kRight1].history = {right_b};
  std::vector<video_filter_state::CropOperation> sized_ops;
  expect_bool("Shift+O maps left crop to each right's size", video_filter_state::copy_crop(sized, sized_ops, CropCopyRequest::LeftToAllRights, false, kLeft, kRight0, 3, 0), true);
  expect_crop("Shift+O 3840 dest is 2x", sized[kRight0].crop, CropState{{200, 100, 3200, 1800}, true});
  expect_crop("Shift+O same-size dest is identical", sized[kRight1].crop, hd_crop);
  expect_crop("Shift+O 1280 dest is edge-mapped", sized[kRight2].crop, mapped_720);
  expect_crop("Shift+O leaves source left", sized[kLeft].crop, hd_crop);
  expect_bool("undo sized Shift+O", video_filter_state::undo_last_crop_operation(sized, sized_ops), true);
  expect_crop("undo sized Shift+O restores Right 0 pixels", sized[kRight0].crop, right_a);
  expect_crop("undo sized Shift+O restores Right 1 pixels", sized[kRight1].crop, right_b);
  expect_crop("undo sized Shift+O restores Right 2 none", sized[kRight2].crop, no_crop);

  sized[kLeft].crop = right_b;
  sized[kLeft].history = {hd_crop, right_b};
  sized[kRight1].crop = hd_crop;
  sized[kRight1].history = {right_b, hd_crop};
  const CropState sized_right0_before_swap_o = sized[kRight0].crop;
  const size_t sized_right0_history_before_swap_o = sized[kRight0].history.size();
  expect_bool("swapped Shift+O maps selected right onto visual right only", video_filter_state::copy_crop(sized, sized_ops, CropCopyRequest::LeftToAllRights, true, kLeft, kRight0, 3, 1), true);
  expect_crop("swapped Shift+O visual right received source crop", sized[kLeft].crop, hd_crop);
  expect_crop("swapped Shift+O does not copy onto other rights", sized[kRight0].crop, sized_right0_before_swap_o);
  expect_size("swapped Shift+O does not push history on other rights", sized[kRight0].history.size(), sized_right0_history_before_swap_o);
  expect_crop("swapped Shift+O source Right 1 unchanged", sized[kRight1].crop, hd_crop);
  expect_crop("swapped Shift+O leaves unselected Right 2", sized[kRight2].crop, no_crop);
  expect_bool("undo swapped sized Shift+O", video_filter_state::undo_last_crop_operation(sized, sized_ops), true);
  expect_crop("undo swapped sized Shift+O restores visual right", sized[kLeft].crop, right_b);
  expect_crop("undo swapped sized Shift+O leaves Right 0", sized[kRight0].crop, sized_right0_before_swap_o);
  expect_crop("undo swapped sized Shift+O leaves Right 1", sized[kRight1].crop, hd_crop);

  sized[kLeft].crop = hd_crop;
  sized[kLeft].history = {hd_crop};

  sized[kRight1].crop = right_b;
  sized[kRight1].history = {right_b};
  expect_bool("Shift+I maps current right onto left", video_filter_state::copy_crop(sized, sized_ops, CropCopyRequest::ActiveRightToLeft, false, kLeft, kRight0, 3, 1), true);
  expect_crop("Shift+I same-size left received Right 1", sized[kLeft].crop, right_b);
  expect_bool("undo sized Shift+I", video_filter_state::undo_last_crop_operation(sized, sized_ops), true);
  expect_crop("undo sized Shift+I restores left pixels", sized[kLeft].crop, hd_crop);
  expect_bool("swapped Shift+I maps left onto 3840 right", video_filter_state::copy_crop(sized, sized_ops, CropCopyRequest::ActiveRightToLeft, true, kLeft, kRight0, 3, 0), true);
  expect_crop("swapped Shift+I Right 0 received 2x left", sized[kRight0].crop, CropState{{200, 100, 3200, 1800}, true});
  expect_crop("swapped Shift+I leaves left", sized[kLeft].crop, hd_crop);
  expect_bool("undo swapped sized Shift+I", video_filter_state::undo_last_crop_operation(sized, sized_ops), true);
  expect_crop("undo swapped sized Shift+I restores Right 0 pixels", sized[kRight0].crop, right_a);

  // Crop-space is the pre-crop insertion size. A destination whose post-filter
  // output is 1280x720 must still map against 1920x1080 crop-space.
  video_filter_state::CropTarget post_scale[2];
  post_scale[kLeft].width = 1920;
  post_scale[kLeft].height = 1080;
  post_scale[kLeft].crop = hd_crop;
  post_scale[kLeft].history = {hd_crop};
  post_scale[kRight0].width = 1920;
  post_scale[kRight0].height = 1080;
  std::vector<video_filter_state::CropOperation> post_scale_ops;
  expect_bool("copy onto dest whose post-filter output is smaller still uses crop-space", video_filter_state::copy_crop(post_scale, post_scale_ops, CropCopyRequest::LeftToAllRights, false, kLeft, kRight0, 1, 0), true);
  expect_crop("post-filter dest maps in 1920x1080 crop-space, not 1280x720 output", post_scale[kRight0].crop, hd_crop);
  expect_bool("1280 dest mapping would have been a different rect", !video_filter_state::crop_states_equal(post_scale[kRight0].crop, mapped_720), true);

  const auto swap_l = video_filter_state::resolve_interactive_crop_targets(true, false, true, 1);
  expect_bool("Shift+L + swap applies to logical right", swap_l.apply_to_right, true);
  expect_bool("Shift+L + swap does not apply to logical left", swap_l.apply_to_left, false);
  expect_size("Shift+L + swap snapshots Right 1, not live selection", swap_l.right_index, 1);
  const auto later_selection = video_filter_state::resolve_interactive_crop_targets(true, false, true, 1);
  expect_size("later Right 2 selection is ignored if snapshot stays Right 1", later_selection.right_index, 1);
  const auto swap_r = video_filter_state::resolve_interactive_crop_targets(false, true, true, 2);
  expect_bool("Shift+R + swap applies to logical left", swap_r.apply_to_left, true);
  expect_bool("Shift+R + swap does not apply to logical right", swap_r.apply_to_right, false);
  const auto no_swap_l = video_filter_state::resolve_interactive_crop_targets(true, false, false, 1);
  expect_bool("Shift+L without swap applies to left", no_swap_l.apply_to_left, true);
  expect_bool("Shift+L without swap does not apply to right", no_swap_l.apply_to_right, false);

  const auto deferred_l = video_filter_state::resolve_interactive_crop_targets(true, false, true, 1);
  const auto deferred_l_if_live = video_filter_state::resolve_interactive_crop_targets(true, false, false, 1);
  expect_bool("deferred Shift+L keeps snapshotted Swap target as logical right", deferred_l.apply_to_right, true);
  expect_bool("deferred Shift+L does not retarget to logical left", deferred_l.apply_to_left, false);
  expect_size("deferred Shift+L keeps snapshotted Right 1", deferred_l.right_index, 1);
  expect_bool("later live unswap would have cropped logical left", deferred_l_if_live.apply_to_left, true);
  const auto deferred_r = video_filter_state::resolve_interactive_crop_targets(false, true, true, 2);
  const auto deferred_r_if_live = video_filter_state::resolve_interactive_crop_targets(false, true, false, 2);
  expect_bool("deferred Shift+R keeps snapshotted Swap target as logical left", deferred_r.apply_to_left, true);
  expect_bool("deferred Shift+R does not retarget to logical right", deferred_r.apply_to_right, false);
  expect_bool("later live unswap would have cropped logical right", deferred_r_if_live.apply_to_right, true);
  const auto deferred_normal_l = video_filter_state::resolve_interactive_crop_targets(true, false, false, 0);
  const auto deferred_normal_l_if_live = video_filter_state::resolve_interactive_crop_targets(true, false, true, 0);
  expect_bool("deferred normal Shift+L keeps logical left", deferred_normal_l.apply_to_left, true);
  expect_bool("deferred normal Shift+L does not apply to right", deferred_normal_l.apply_to_right, false);
  expect_bool("later live Swap would have cropped logical right", deferred_normal_l_if_live.apply_to_right, true);

  const auto o_normal = video_filter_state::resolve_crop_copy(CropCopyRequest::LeftToAllRights, false, 1, 3);
  expect_bool("Shift+O normal source is logical left", o_normal.source_is_left, true);
  expect_bool("Shift+O normal destinations are all rights", o_normal.copy_to_all_rights, true);
  const auto o_swap = video_filter_state::resolve_crop_copy(CropCopyRequest::LeftToAllRights, true, 1, 3);
  expect_bool("Shift+O swapped source is current right", o_swap.source_is_left, false);
  expect_size("Shift+O swapped source is snapshotted Right 1", o_swap.source_right_index, 1);
  expect_bool("Shift+O swapped destinations are not all rights", o_swap.copy_to_all_rights, false);
  expect_bool("Shift+O swapped copies only onto visual right (logical left)", o_swap.dest_is_left, true);
  expect_bool("Shift+O normal does not copy onto logical left", o_normal.dest_is_left, false);
  const auto i_normal = video_filter_state::resolve_crop_copy(CropCopyRequest::ActiveRightToLeft, false, 1, 3);
  expect_bool("Shift+I normal source is current right", i_normal.source_is_left, false);
  expect_size("Shift+I normal source is snapshotted Right 1", i_normal.source_right_index, 1);
  expect_bool("Shift+I normal dest is logical left", i_normal.dest_is_left, true);
  const auto i_swap = video_filter_state::resolve_crop_copy(CropCopyRequest::ActiveRightToLeft, true, 1, 3);
  expect_bool("Shift+I swapped source is logical left", i_swap.source_is_left, true);
  expect_bool("Shift+I swapped dest is current right", i_swap.dest_is_left, false);
  expect_size("Shift+I swapped dest is snapshotted Right 1", i_swap.dest_right_index, 1);

  const PendingCropCopy deferred_o{CropCopyRequest::LeftToAllRights, 1, true};
  const auto deferred_o_plan = video_filter_state::resolve_crop_copy(deferred_o.request, deferred_o.swap_left_right, deferred_o.right_target_index, 3);
  const auto deferred_o_if_live = video_filter_state::resolve_crop_copy(deferred_o.request, false, deferred_o.right_target_index, 3);
  expect_bool("deferred Shift+O keeps snapshotted Swap source as current right", deferred_o_plan.source_is_left, false);
  expect_size("deferred Shift+O keeps snapshotted Right 1", deferred_o_plan.source_right_index, 1);
  expect_bool("deferred Shift+O keeps snapshotted visual-right dest", deferred_o_plan.dest_is_left, true);
  expect_bool("deferred Shift+O does not copy onto other rights", deferred_o_plan.copy_to_all_rights, false);
  expect_bool("later live unswap would have used logical left as source", deferred_o_if_live.source_is_left, true);
  expect_bool("later live unswap would not copy onto logical left", deferred_o_if_live.dest_is_left, false);
  const PendingCropCopy deferred_i{CropCopyRequest::ActiveRightToLeft, 1, true};
  const auto deferred_i_plan = video_filter_state::resolve_crop_copy(deferred_i.request, deferred_i.swap_left_right, deferred_i.right_target_index, 3);
  const auto deferred_i_if_live = video_filter_state::resolve_crop_copy(deferred_i.request, false, deferred_i.right_target_index, 3);
  expect_bool("deferred Shift+I keeps snapshotted Swap source as logical left", deferred_i_plan.source_is_left, true);
  expect_bool("deferred Shift+I keeps snapshotted dest as Right 1", deferred_i_plan.dest_is_left, false);
  expect_size("deferred Shift+I dest stays snapshotted Right 1", deferred_i_plan.dest_right_index, 1);
  expect_bool("later live unswap would have copied Right 1 to left", deferred_i_if_live.source_is_left, false);
  expect_bool("later live unswap would have used left as dest", deferred_i_if_live.dest_is_left, true);

  video_filter_state::CropTarget visual[4];
  std::vector<video_filter_state::CropOperation> visual_ops;
  expect_bool("visual seed left", video_filter_state::apply_crop_to_indices(visual, visual_ops, left_only, 1, left_crop), true);
  expect_bool("visual seed Right 0 A", video_filter_state::apply_crop_to_indices(visual, visual_ops, right0_only, 1, right_a), true);
  expect_bool("visual seed Right 1 B", video_filter_state::apply_crop_to_indices(visual, visual_ops, right1_only, 1, right_b), true);
  const size_t left_history_before_swap_o = visual[kLeft].history.size();
  const size_t right0_history_before_swap_o = visual[kRight0].history.size();
  const size_t right1_history_before_swap_o = visual[kRight1].history.size();
  const size_t right2_history_before_swap_o = visual[kRight2].history.size();

  expect_bool("Shift+O normal copies logical left to all rights", video_filter_state::copy_crop(visual, visual_ops, CropCopyRequest::LeftToAllRights, false, kLeft, kRight0, 3, 0), true);
  expect_operation("Shift+O normal records all rights", visual_ops.back(), {kRight0, kRight1, kRight2});
  expect_crop("Shift+O normal Right 0 received left", visual[kRight0].crop, left_crop);
  expect_crop("Shift+O normal Right 1 received left", visual[kRight1].crop, left_crop);
  expect_crop("Shift+O normal Right 2 received left", visual[kRight2].crop, left_crop);
  expect_crop("Shift+O normal leaves logical left unchanged", visual[kLeft].crop, left_crop);
  expect_bool("undo Shift+O normal after later swap/selection", video_filter_state::undo_last_crop_operation(visual, visual_ops), true);
  expect_crop("Shift+O normal undo restores Right 0", visual[kRight0].crop, right_a);
  expect_crop("Shift+O normal undo restores Right 1", visual[kRight1].crop, right_b);
  expect_crop("Shift+O normal undo restores Right 2", visual[kRight2].crop, no_crop);
  expect_crop("Shift+O normal undo leaves left", visual[kLeft].crop, left_crop);

  expect_bool("Shift+O swapped copies visual left onto visual right only", video_filter_state::copy_crop(visual, visual_ops, CropCopyRequest::LeftToAllRights, true, kLeft, kRight0, 3, 1), true);
  expect_operation("Shift+O swapped records visual right only", visual_ops.back(), {kLeft});
  expect_crop("Shift+O swapped visual right (logical left) received source crop", visual[kLeft].crop, right_b);
  expect_crop("Shift+O swapped leaves other Right 0", visual[kRight0].crop, right_a);
  expect_crop("Shift+O swapped source Right 1 unchanged", visual[kRight1].crop, right_b);
  expect_crop("Shift+O swapped leaves other Right 2", visual[kRight2].crop, no_crop);
  expect_size("Shift+O swapped records visual right (logical left)", visual[kLeft].history.size(), left_history_before_swap_o + 1);
  expect_size("Shift+O swapped does not push history on other Right 0", visual[kRight0].history.size(), right0_history_before_swap_o);
  expect_size("Shift+O swapped does not push history on source right", visual[kRight1].history.size(), right1_history_before_swap_o);
  expect_size("Shift+O swapped does not push history on other Right 2", visual[kRight2].history.size(), right2_history_before_swap_o);
  expect_bool("undo Shift+O swapped after later swap/selection", video_filter_state::undo_last_crop_operation(visual, visual_ops), true);
  expect_crop("Shift+O swapped undo restores left", visual[kLeft].crop, left_crop);
  expect_crop("Shift+O swapped undo leaves Right 0", visual[kRight0].crop, right_a);
  expect_crop("Shift+O swapped undo leaves source Right 1", visual[kRight1].crop, right_b);
  expect_crop("Shift+O swapped undo leaves Right 2", visual[kRight2].crop, no_crop);

  expect_bool("Shift+I normal copies current right to left", video_filter_state::copy_crop(visual, visual_ops, CropCopyRequest::ActiveRightToLeft, false, kLeft, kRight0, 3, 0), true);
  expect_operation("Shift+I normal records left only", visual_ops.back(), {kLeft});
  expect_crop("Shift+I normal left received Right 0", visual[kLeft].crop, right_a);
  expect_crop("Shift+I normal leaves Right 0", visual[kRight0].crop, right_a);
  expect_crop("Shift+I normal leaves Right 1", visual[kRight1].crop, right_b);
  expect_bool("undo Shift+I normal after later swap/selection", video_filter_state::undo_last_crop_operation(visual, visual_ops), true);
  expect_crop("Shift+I normal undo restores left", visual[kLeft].crop, left_crop);
  expect_crop("Shift+I normal undo leaves Right 0", visual[kRight0].crop, right_a);
  expect_crop("Shift+I normal undo leaves Right 1", visual[kRight1].crop, right_b);

  expect_bool("Shift+I swapped copies left to current right", video_filter_state::copy_crop(visual, visual_ops, CropCopyRequest::ActiveRightToLeft, true, kLeft, kRight0, 3, 1), true);
  expect_operation("Shift+I swapped records snapshotted Right 1", visual_ops.back(), {kRight1});
  expect_crop("Shift+I swapped Right 1 received left", visual[kRight1].crop, left_crop);
  expect_crop("Shift+I swapped leaves logical left", visual[kLeft].crop, left_crop);
  expect_crop("Shift+I swapped leaves Right 0", visual[kRight0].crop, right_a);
  expect_bool("undo Shift+I swapped after later swap/selection", video_filter_state::undo_last_crop_operation(visual, visual_ops), true);
  expect_crop("Shift+I swapped undo restores Right 1", visual[kRight1].crop, right_b);
  expect_crop("Shift+I swapped undo leaves left", visual[kLeft].crop, left_crop);
  expect_crop("Shift+I swapped undo leaves Right 0", visual[kRight0].crop, right_a);
  expect_crop("Shift+I swapped undo leaves Right 2", visual[kRight2].crop, no_crop);

  expect_bool("selected right in range is valid", video_filter_state::selected_right(1, 3).valid, true);
  expect_size("selected right in range", video_filter_state::selected_right(1, 3).index, 1);
  expect_size("selected right first", video_filter_state::selected_right(0, 2).index, 0);
  expect_size("selected right clamps high", video_filter_state::selected_right(9, 3).index, 2);
  expect_bool("selected right empty is invalid", video_filter_state::selected_right(4, 0).valid, false);
  expect_size("selected right empty does not invent an index", video_filter_state::selected_right(4, 0).index, 0);

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }

  std::printf("All video filter state tests passed\n");
  return EXIT_SUCCESS;
}
