#include "core_types.h"
#include "keyframe_index.h"
#include "keyframe_index_probe.h"
#include "playback_keyframe.h"
#include "playback_seek.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>
extern "C" {
#include <libavformat/avformat.h>
}

static int failures = 0;

static constexpr float kPosTol = 2e-3F;
static constexpr float kPresentationLatticeTol = 0.04F;

static void expect_i(const char* label, const int got, const int expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: expected %d, got %d\n", label, expected, got);
    failures++;
    return;
  }
  std::printf("PASS %s -> %d\n", label, got);
}

static void expect_near(const char* label, const float got, const float expected) {
  if (!(std::fabs(got - expected) <= kPosTol)) {
    std::fprintf(stderr, "FAIL %s: expected %.8g, got %.8g\n", label, static_cast<double>(expected), static_cast<double>(got));
    failures++;
    return;
  }
  std::printf("PASS %s -> %.8g\n", label, static_cast<double>(got));
}

static void expect_i64(const char* label, const int64_t got, const int64_t expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: expected %lld, got %lld\n", label, static_cast<long long>(expected), static_cast<long long>(got));
    failures++;
    return;
  }
  std::printf("PASS %s -> %lld\n", label, static_cast<long long>(got));
}

static void expect_status(const char* label, const playback_keyframe::AdjacentKeyframeStatus got, const playback_keyframe::AdjacentKeyframeStatus expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: unexpected status %d\n", label, static_cast<int>(got));
    failures++;
    return;
  }
  std::printf("PASS %s status\n", label);
}

static void expect_bool(const char* label, const bool got, const bool expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: expected %d, got %d\n", label, static_cast<int>(expected), static_cast<int>(got));
    failures++;
    return;
  }
  std::printf("PASS %s -> %s\n", label, got ? "true" : "false");
}

static playback_seek::SeekRequest sample_seek_request() {
  playback_seek::SeekRequest request;
  request.seek_relative = 0.0F;
  request.seek_from_start = false;
  request.force_seek_current_position = false;
  request.all_sides_multi_frame = true;
  request.shift_right_frames = 0;
  request.shortest_duration = 10.0F;
  request.left_pts = 500000;
  request.unadjusted_static_right_time_shift = 80000;
  request.time_shift_multiplier = AVRational{3, 2};
  request.left.start_time = 0.0F;
  request.left.first_pts = 0;
  request.left.delta_pts = 40000;
  request.left.single_frame = false;

  playback_seek::SeekSideInput right;
  right.start_time = 0.0F;
  right.first_pts = 0;
  right.delta_pts = 40000;
  right.single_frame = false;
  request.rights.push_back(right);
  return request;
}

static std::string fixture_path(int argc, char** argv) {
  if (argc >= 2) {
    return argv[1];
  }
  const char* env = std::getenv("KEYFRAME_TEST_FIXTURE");
  return env != nullptr ? env : "";
}

static std::vector<float> keyframe_demuxer_positions_from_snapshot(const playback_keyframe::KeyframeIndexSnapshot& snapshot) {
  std::vector<float> positions;
  for (const playback_keyframe::IndexEntry& entry : snapshot.entries) {
    if ((entry.flags & AVINDEX_KEYFRAME) == 0) {
      continue;
    }
    const int64_t presentation_timestamp = playback_keyframe::index_entry_presentation_timestamp(snapshot, entry);
    positions.push_back(playback_keyframe::stream_timestamp_to_demuxer_seconds(presentation_timestamp, snapshot.time_base, snapshot.video_start_time_seconds));
  }
  return positions;
}

static void test_synthetic_reference_index_ignores_non_keyframes() {
  playback_keyframe::KeyframeIndexSnapshot snapshot;
  snapshot.presentation_mapping_verified = true;
  snapshot.time_base = AVRational{1, 25};
  snapshot.entries = {
      {-2, AVINDEX_KEYFRAME},
      {5, 0},
      {8, 0},
      {10, AVINDEX_KEYFRAME},
      {16, 0},
      {22, AVINDEX_KEYFRAME},
  };

  constexpr int64_t kPresentationOffset = 2;
  for (playback_keyframe::IndexEntry& entry : snapshot.entries) {
    if ((entry.flags & AVINDEX_KEYFRAME) != 0) {
      entry.presentation_timestamp = entry.timestamp + kPresentationOffset;
    }
  }

  const int64_t between_first_and_second_key = playback_keyframe::demuxer_seconds_to_stream_timestamp(0.24F, snapshot.time_base, 0.0F);
  expect_i("reference index skips non-keyframes", playback_keyframe::find_reference_index(snapshot, between_first_and_second_key), 0);

  const int64_t after_second_key_pres = playback_keyframe::index_entry_presentation_timestamp(snapshot, snapshot.entries[3]);
  expect_i("reference index lands on prior keyframe", playback_keyframe::find_reference_index(snapshot, after_second_key_pres), 3);
}

static void test_keyframe_navigation_cursor_and_reference() {
  playback_keyframe::KeyframeNavigationCursor cursor;
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;

  const float displayed_left = 1.041667F;
  const float from_display_left = playback_keyframe::keyframe_navigation_reference_comparison_timeline(cursor, 0, request, displayed_left, true, 0.0F, false, request.shortest_duration);
  expect_near("reference prefers displayed left", from_display_left, displayed_left);

  playback_seek::SeekRequest unity_request = sample_seek_request();
  unity_request.left_pts = 0;
  unity_request.unadjusted_static_right_time_shift = 0;
  unity_request.time_shift_multiplier = AVRational{1, 1};
  const float displayed_right = 1.041667F;
  const float from_display_right = playback_keyframe::keyframe_navigation_reference_comparison_timeline(cursor, 0, unity_request, 0.0F, false, displayed_right, true, unity_request.shortest_duration);
  const playback_keyframe::MappedPosition expected_mapped =
      playback_keyframe::comparison_timeline_for_right_demuxer_position(unity_request, 0, displayed_right, unity_request.shortest_duration);
  expect_bool("mapped right for reference ok", expected_mapped.ok, true);
  if (expected_mapped.ok) {
    expect_near("reference uses mapped right when left not shown", from_display_right, expected_mapped.seconds);
  }

  playback_keyframe::commit_keyframe_navigation_cursor(cursor, 0, 2.083333F);
  const float from_cursor = playback_keyframe::keyframe_navigation_reference_comparison_timeline(cursor, 0, request, displayed_left, true, displayed_right, true, request.shortest_duration);
  expect_near("reference prefers navigation cursor", from_cursor, 2.083333F);

  playback_keyframe::reset_keyframe_navigation_cursor(cursor);
  expect_bool("cursor reset", cursor.valid, false);

  playback_keyframe::commit_keyframe_navigation_cursor(cursor, 1, 3.125F);
  const float wrong_index = playback_keyframe::keyframe_navigation_reference_comparison_timeline(cursor, 0, request, displayed_left, true, displayed_right, true, request.shortest_duration);
  expect_near("cursor ignored for other active right index", wrong_index, displayed_left);
}

static void test_keyframe_navigation_cursor_walk_sparse_left() {
  playback_keyframe::KeyframeIndexSnapshot snapshot;
  snapshot.seekable = true;
  snapshot.presentation_mapping_verified = true;
  snapshot.time_base = AVRational{1, 48};
  snapshot.video_start_time_seconds = 0.0F;

  const std::vector<float> lattice = {0.0F, 1.041667F, 2.083333F, 3.125F, 4.166667F, 5.208333F};
  for (const float position : lattice) {
    playback_keyframe::IndexEntry entry{};
    entry.flags = AVINDEX_KEYFRAME;
    entry.presentation_timestamp = playback_keyframe::demuxer_seconds_to_stream_timestamp(position, snapshot.time_base, 0.0F);
    snapshot.entries.push_back(entry);
  }

  playback_keyframe::KeyframeNavigationCursor cursor;
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;

  float reference = 0.0F;
  for (size_t step = 1; step < lattice.size(); ++step) {
    const playback_keyframe::AdjacentKeyframe next = playback_keyframe::find_adjacent_keyframe(snapshot, +1, reference);
    expect_status("cursor walk forward", next.status, playback_keyframe::AdjacentKeyframeStatus::Ok);
    if (next.status != playback_keyframe::AdjacentKeyframeStatus::Ok) {
      break;
    }
    expect_near("cursor walk forward landing", next.demuxer_position_seconds, lattice[step]);
    playback_keyframe::commit_keyframe_navigation_cursor(cursor, 0, next.demuxer_position_seconds);
    reference = playback_keyframe::keyframe_navigation_reference_comparison_timeline(cursor, 0, request, 0.0F, false, 0.0F, false, request.shortest_duration);
    expect_near("cursor walk reference", reference, lattice[step]);
  }
}

static void test_synthetic_pts_dts_mapping_navigation() {
  playback_keyframe::KeyframeIndexSnapshot snapshot;
  snapshot.seekable = true;
  snapshot.presentation_mapping_verified = true;
  snapshot.time_base = AVRational{1, 25};
  snapshot.video_start_time_seconds = 0.0F;
  snapshot.entries = {
      {-2, AVINDEX_KEYFRAME},
      {4, 0},
      {7, 0},
      {10, AVINDEX_KEYFRAME},
      {16, 0},
      {19, 0},
      {22, AVINDEX_KEYFRAME},
      {28, 0},
      {34, AVINDEX_KEYFRAME},
      {40, 0},
      {46, AVINDEX_KEYFRAME},
      {52, 0},
      {58, AVINDEX_KEYFRAME},
  };

  constexpr int64_t kPresentationOffset = 2;
  for (playback_keyframe::IndexEntry& entry : snapshot.entries) {
    if ((entry.flags & AVINDEX_KEYFRAME) != 0) {
      entry.presentation_timestamp = entry.timestamp + kPresentationOffset;
    }
  }

  const float lattice[] = {0.0F, 0.48F, 0.96F, 1.44F, 1.92F, 2.40F};

  for (size_t i = 0; i + 1 < sizeof(lattice) / sizeof(lattice[0]); ++i) {
    const playback_keyframe::AdjacentKeyframe next = playback_keyframe::find_adjacent_keyframe(snapshot, +1, lattice[i]);
    expect_status("synthetic forward step", next.status, playback_keyframe::AdjacentKeyframeStatus::Ok);
    if (next.status == playback_keyframe::AdjacentKeyframeStatus::Ok) {
      expect_near("synthetic forward landing", next.demuxer_position_seconds, lattice[i + 1]);
    }
  }

  const playback_keyframe::AdjacentKeyframe at_zero = playback_keyframe::find_adjacent_keyframe(snapshot, +1, 0.0F);
  expect_status("synthetic forward from 0", at_zero.status, playback_keyframe::AdjacentKeyframeStatus::Ok);
  if (at_zero.status == playback_keyframe::AdjacentKeyframeStatus::Ok) {
    expect_near("synthetic forward from 0 landing", at_zero.demuxer_position_seconds, 0.48F);
  }

  const playback_keyframe::AdjacentKeyframe back_from_240 = playback_keyframe::find_adjacent_keyframe(snapshot, -1, 2.40F);
  expect_status("synthetic back from 2.40", back_from_240.status, playback_keyframe::AdjacentKeyframeStatus::Ok);
  expect_near("synthetic back to 1.92", back_from_240.demuxer_position_seconds, 1.92F);

  const playback_keyframe::AdjacentKeyframe back_from_192 = playback_keyframe::find_adjacent_keyframe(snapshot, -1, 1.92F);
  expect_status("synthetic back from 1.92", back_from_192.status, playback_keyframe::AdjacentKeyframeStatus::Ok);
  expect_near("synthetic back to 1.44", back_from_192.demuxer_position_seconds, 1.44F);
}

static void test_nonuniform_mapping_rejects_unprobed_pts() {
  playback_keyframe::KeyframeIndexSnapshot snapshot;
  snapshot.seekable = true;
  snapshot.presentation_mapping_verified = true;
  snapshot.time_base = AVRational{1, 25};
  snapshot.video_start_time_seconds = 0.0F;
  snapshot.entries = {
      {8, AVINDEX_KEYFRAME},
      {30, AVINDEX_KEYFRAME},
      {52, AVINDEX_KEYFRAME},
  };
  snapshot.entries[0].presentation_timestamp = 0;
  snapshot.entries[2].presentation_timestamp = 54;

  expect_i64("unprobed middle entry", playback_keyframe::index_entry_presentation_timestamp(snapshot, snapshot.entries[1]), AV_NOPTS_VALUE);

  const playback_keyframe::AdjacentKeyframe blocked = playback_keyframe::find_adjacent_keyframe(snapshot, +1, 0.0F);
  expect_status("nonuniform blocks unprobed neighbor", blocked.status, playback_keyframe::AdjacentKeyframeStatus::Unavailable);

  snapshot.entries[1].presentation_timestamp = 32;
  const playback_keyframe::AdjacentKeyframe next = playback_keyframe::find_adjacent_keyframe(snapshot, +1, 0.0F);
  expect_status("nonuniform navigates after probe", next.status, playback_keyframe::AdjacentKeyframeStatus::Ok);
  if (next.status == playback_keyframe::AdjacentKeyframeStatus::Ok) {
    expect_near("nonuniform middle probed landing", next.demuxer_position_seconds, 1.28F);
  }
}

static void test_nonuniform_different_dts_pts_offsets() {
  playback_keyframe::KeyframeIndexSnapshot snapshot;
  snapshot.seekable = true;
  snapshot.presentation_mapping_verified = true;
  snapshot.time_base = AVRational{1, 25};
  snapshot.video_start_time_seconds = 0.0F;
  snapshot.entries = {
      {0, AVINDEX_KEYFRAME},
      {25, AVINDEX_KEYFRAME},
      {50, AVINDEX_KEYFRAME},
  };
  snapshot.entries[0].presentation_timestamp = 0;
  snapshot.entries[1].presentation_timestamp = 27;
  snapshot.entries[2].presentation_timestamp = 52;

  expect_near("offset entry 0", playback_keyframe::stream_timestamp_to_demuxer_seconds(snapshot.entries[0].presentation_timestamp, snapshot.time_base, 0.0F), 0.0F);
  expect_near("offset entry 1", playback_keyframe::stream_timestamp_to_demuxer_seconds(snapshot.entries[1].presentation_timestamp, snapshot.time_base, 0.0F), 1.08F);
  expect_near("offset entry 2", playback_keyframe::stream_timestamp_to_demuxer_seconds(snapshot.entries[2].presentation_timestamp, snapshot.time_base, 0.0F), 2.08F);

  const playback_keyframe::AdjacentKeyframe next = playback_keyframe::find_adjacent_keyframe(snapshot, +1, 1.0F);
  expect_status("late timeline neighbor", next.status, playback_keyframe::AdjacentKeyframeStatus::Ok);
  if (next.status == playback_keyframe::AdjacentKeyframeStatus::Ok) {
    expect_near("late timeline neighbor lands", next.demuxer_position_seconds, 1.08F);
  }
}

static playback_keyframe::AdjacentKeyframe synthetic_adjacent_ok(const float position) {
  playback_keyframe::AdjacentKeyframe adjacent;
  adjacent.status = playback_keyframe::AdjacentKeyframeStatus::Ok;
  adjacent.demuxer_position_seconds = position;
  return adjacent;
}

static playback_keyframe::AdjacentKeyframe synthetic_adjacent_at_boundary() {
  playback_keyframe::AdjacentKeyframe adjacent;
  adjacent.status = playback_keyframe::AdjacentKeyframeStatus::AtBoundary;
  return adjacent;
}

static playback_keyframe::AdjacentKeyframe synthetic_adjacent_unavailable() {
  playback_keyframe::AdjacentKeyframe adjacent;
  adjacent.status = playback_keyframe::AdjacentKeyframeStatus::Unavailable;
  return adjacent;
}

static void test_union_empty_right_unmapped_left_boundary_is_unavailable() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;
  request.rights.clear();

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      +1, 1.0F, synthetic_adjacent_at_boundary(), synthetic_adjacent_ok(2.0F), true, true, request, 0, request.shortest_duration);
  expect_bool("unmapped right with left boundary not at boundary", selection.at_boundary, false);
  expect_bool("unmapped right with left boundary unavailable", selection.found, false);
}

static void test_union_backward_shifted_right_before_start_is_boundary(const int64_t static_shift_avtime) {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;
  request.unadjusted_static_right_time_shift = static_shift_avtime;
  request.time_shift_multiplier = AVRational{1, 1};

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      -1, 0.0F, synthetic_adjacent_at_boundary(), synthetic_adjacent_ok(0.0F), true, true, request, 0, request.shortest_duration);
  const char* label = static_shift_avtime == 40000   ? "+40ms backward at origin is boundary"
                      : static_shift_avtime == 0       ? "zero shift backward at origin is boundary"
                      : "-40ms backward at origin is boundary";
  expect_bool(label, selection.at_boundary, true);
  expect_bool("shifted backward at origin not found", selection.found, false);
}

static void test_union_backward_shifted_right_before_start_classifications() {
  test_union_backward_shifted_right_before_start_is_boundary(40000);
  test_union_backward_shifted_right_before_start_is_boundary(0);
  test_union_backward_shifted_right_before_start_is_boundary(-40000);
}

static void test_union_forward_right_unmapped_stays_unavailable() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;
  request.rights.clear();

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      +1, 0.0F, synthetic_adjacent_at_boundary(), synthetic_adjacent_ok(1.0F), true, true, request, 0, request.shortest_duration);
  expect_bool("forward right unmapped not at boundary", selection.at_boundary, false);
  expect_bool("forward right unmapped unavailable", selection.found, false);
}

static void test_union_backward_right_unmapped_no_rights_stays_unavailable() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;
  request.rights.clear();

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      -1, 0.0F, synthetic_adjacent_at_boundary(), synthetic_adjacent_ok(-0.04F), true, true, request, 0, request.shortest_duration);
  expect_bool("backward right unmapped no rights not at boundary", selection.at_boundary, false);
  expect_bool("backward right unmapped no rights unavailable", selection.found, false);
}

static void test_union_backward_reachable_right_key_still_selected() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = static_cast<int64_t>(1.041667F / AV_TIME_TO_SEC);
  request.unadjusted_static_right_time_shift = 40000;
  request.time_shift_multiplier = AVRational{1, 1};

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      -1, 1.041667F, synthetic_adjacent_ok(0.0F), synthetic_adjacent_ok(0.0F), true, true, request, 0, request.shortest_duration);
  expect_bool("backward with reachable union key finds candidate", selection.found, true);
  if (selection.found) {
    expect_near("backward union lands on first key", selection.candidate.comparison_timeline_position, 0.0F);
  }
}

static void test_union_backward_left_maps_before_negative_shifted_right_start() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = static_cast<int64_t>(0.083 / AV_TIME_TO_SEC);
  request.unadjusted_static_right_time_shift = -80000;
  request.time_shift_multiplier = AVRational{1, 1};

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      -1, 0.083F, synthetic_adjacent_ok(0.04F), synthetic_adjacent_at_boundary(), true, true, request, 0, request.shortest_duration);
  expect_bool("negative shift left 0.04 maps right before start", selection.at_boundary, true);
  expect_bool("negative shift left 0.04 not selected", selection.found, false);
}

static void test_union_backward_negative_shift_reachable_left_still_selected() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = static_cast<int64_t>(1.041667F / AV_TIME_TO_SEC);
  request.unadjusted_static_right_time_shift = -80000;
  request.time_shift_multiplier = AVRational{1, 1};

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      -1, 1.041667F, synthetic_adjacent_ok(0.083333F), synthetic_adjacent_at_boundary(), true, true, request, 0, request.shortest_duration);
  expect_bool("negative shift backward reachable left found", selection.found, true);
  if (selection.found) {
    expect_near("negative shift backward reachable left lands", selection.candidate.comparison_timeline_position, 0.083333F);
  }
}

static void test_union_empty_right_unavailable_left_boundary_is_unavailable() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      +1, 1.0F, synthetic_adjacent_at_boundary(), synthetic_adjacent_unavailable(), true, true, request, 0, request.shortest_duration);
  expect_bool("right unavailable left boundary not union boundary", selection.at_boundary, false);
  expect_bool("right unavailable left boundary not found", selection.found, false);
}

static void test_union_empty_both_at_boundary() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      +1, 2.0F, synthetic_adjacent_at_boundary(), synthetic_adjacent_at_boundary(), true, true, request, 0, request.shortest_duration);
  expect_bool("both indexed at boundary", selection.at_boundary, true);
  expect_bool("both indexed at boundary not found", selection.found, false);
}

static void test_union_left_candidate_right_unavailable_query() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      +1, 0.0F, synthetic_adjacent_ok(1.0F), synthetic_adjacent_unavailable(), true, true, request, 0, request.shortest_duration);
  expect_bool("left works when right query unavailable", selection.found, true);
  if (selection.found) {
    expect_bool("left owner when right unavailable", selection.candidate.owner == SideType::Left, true);
  }
}

static void test_left_single_frame_inverse_unsupported() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;
  request.left.single_frame = true;
  request.unadjusted_static_right_time_shift = 0;
  request.time_shift_multiplier = AVRational{1, 1};

  expect_bool("single-frame left nav unsupported", playback_keyframe::keyframe_navigation_seek_plan_supported(request, 0), false);
  const playback_keyframe::SeekRelativeSolveResult solved = playback_keyframe::seek_relative_for_left_demuxer_position(request, 2.0F, request.shortest_duration);
  expect_bool("single-frame left inverse fails closed", solved.ok, false);
}

static void test_left_inverse_clamp_first_pts() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;
  request.left.first_pts = 500000;
  request.left.start_time = 0.0F;
  request.unadjusted_static_right_time_shift = 0;
  request.time_shift_multiplier = AVRational{1, 1};

  const float min_pos = 0.5F;
  const playback_keyframe::SeekRelativeSolveResult at_min = playback_keyframe::seek_relative_for_left_demuxer_position(request, min_pos, request.shortest_duration);
  expect_bool("left inverse at first_pts clamp", at_min.ok, true);
  expect_near("left inverse lands at clamp", playback_keyframe::left_target_position_for_seek_relative(request, at_min.seek_relative), min_pos);

  const playback_keyframe::SeekRelativeSolveResult above = playback_keyframe::seek_relative_for_left_demuxer_position(request, 1.25F, request.shortest_duration);
  expect_bool("left inverse above clamp", above.ok, true);
  expect_near("left inverse above clamp lands", playback_keyframe::left_target_position_for_seek_relative(request, above.seek_relative), 1.25F);

  const playback_keyframe::SeekRelativeSolveResult below = playback_keyframe::seek_relative_for_left_demuxer_position(request, 0.1F, request.shortest_duration);
  expect_bool("left inverse below clamp fails", below.ok, false);
}

static void test_union_keyframe_prefers_nearest_forward() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;
  request.unadjusted_static_right_time_shift = 0;
  request.time_shift_multiplier = AVRational{1, 1};

  const float reference = 0.5F;
  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      +1, reference, synthetic_adjacent_ok(1.0F), synthetic_adjacent_ok(2.0F), true, true, request, 0, request.shortest_duration);
  expect_bool("union forward found", selection.found, true);
  if (selection.found) {
    expect_near("union forward nearest on comparison timeline", selection.candidate.comparison_timeline_position, 1.0F);
    expect_bool("union forward owner is left", selection.candidate.owner == SideType::Left, true);
  }
}

static void test_union_keyframe_coincident_dedupes() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;
  request.unadjusted_static_right_time_shift = 0;
  request.time_shift_multiplier = AVRational{1, 1};

  const float reference = 0.0F;
  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      +1, reference, synthetic_adjacent_ok(1.0F), synthetic_adjacent_ok(1.0F), true, true, request, 0, request.shortest_duration);
  expect_bool("union coincident forward found", selection.found, true);
  if (selection.found) {
    expect_near("union coincident lands once", selection.candidate.comparison_timeline_position, 1.0F);
    expect_bool("union coincident prefers left owner", selection.candidate.owner == SideType::Left, true);
  }
}

static void test_union_keyframe_respects_left_start_offset() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;
  request.left.start_time = 0.5F;
  request.unadjusted_static_right_time_shift = 0;
  request.time_shift_multiplier = AVRational{1, 1};

  const float reference = 0.5F;
  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      +1, reference, synthetic_adjacent_ok(1.5F), synthetic_adjacent_at_boundary(), true, false, request, 0, request.shortest_duration);
  expect_bool("union left start offset forward", selection.found, true);
  if (selection.found) {
    expect_near("union left start offset lands", selection.candidate.comparison_timeline_position, 1.5F);
  }
}

static void test_union_keyframe_no_next_at_boundary() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      +1, 2.0F, synthetic_adjacent_at_boundary(), synthetic_adjacent_at_boundary(), true, true, request, 0, request.shortest_duration);
  expect_bool("union no next at boundary", selection.at_boundary, true);
  expect_bool("union no next not found", selection.found, false);
}

static void test_union_right_adjacent_reference_skips_current_key() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = static_cast<int64_t>(1.002 / AV_TIME_TO_SEC);
  request.unadjusted_static_right_time_shift = 40000;
  request.time_shift_multiplier = AVRational{1, 1};

  const float mapped = playback_keyframe::current_right_demuxer_position(request, 0);
  const float biased = playback_keyframe::union_right_adjacent_reference_demuxer_seconds(request, 0, 1.002F, +1, mapped, request.shortest_duration);
  expect_bool("right adjacent ref after landed vertex advances", biased > mapped + 1e-3F, true);
  expect_bool("right adjacent ref does not skip a 48fps frame", biased < mapped + 1.0F / 48.0F, true);

  const playback_keyframe::UnionKeyframeSelection next = playback_keyframe::select_union_keyframe_candidate(
      +1, 1.002F, synthetic_adjacent_ok(5.208333F), synthetic_adjacent_ok(2.083333F), true, true, request, 0, request.shortest_duration);
  expect_bool("dense right union step after shifted vertex", next.found, true);
  if (next.found) {
    expect_bool("next dense right owner", next.candidate.owner == SideType::Right, true);
    expect_near("next dense right comparison", next.candidate.comparison_timeline_position, 2.043333F);
  }
}

// In the sparse-left / dense-right case, later right-owned keyframe steps must
// use the navigation cursor, not the potentially lagging displayed left PTS.
static void test_union_sparse_left_successive_right_owned_steps() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.unadjusted_static_right_time_shift = 40000;
  request.time_shift_multiplier = AVRational{1, 1};

  playback_keyframe::KeyframeNavigationCursor cursor;
  for (const float right_key : {3.125F, 4.166667F}) {
    const float prior_cursor = right_key - 1.041667F - 0.04F;
    // After a right-owned seek, the sparse left display can lag behind the cursor.
    const float displayed_left = cursor.valid ? 2.604F : prior_cursor;
    request.left_pts = static_cast<int64_t>(displayed_left / AV_TIME_TO_SEC);
    const float reference = playback_keyframe::keyframe_navigation_reference_comparison_timeline(
        cursor, 0, request, displayed_left, true, 0.0F, false, request.shortest_duration);
    expect_near("sparse left cursor independent of lagging left frame", reference, prior_cursor);
    const playback_keyframe::UnionKeyframeSelection next = playback_keyframe::select_union_keyframe_candidate(
        +1, reference, synthetic_adjacent_ok(5.208333F), synthetic_adjacent_ok(right_key), true, true, request, 0, request.shortest_duration);
    expect_bool("sparse left successive right step found", next.found, true);
    if (next.found) {
      expect_bool("sparse left successive right owner", next.candidate.owner == SideType::Right, true);
      expect_near("sparse left successive right comparison", next.candidate.comparison_timeline_position, right_key - 0.04F);
      playback_keyframe::commit_keyframe_navigation_cursor(cursor, 0, next.candidate.comparison_timeline_position);
    }
  }
}

static void test_union_forward_from_common_5104_with_40ms_shift() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.time_shift_multiplier = AVRational{1, 1};
  request.unadjusted_static_right_time_shift = 40000;
  request.left_pts = static_cast<int64_t>(5.104 / AV_TIME_TO_SEC);

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      +1, 5.104F, synthetic_adjacent_ok(5.208333F), synthetic_adjacent_ok(5.208333F), true, true, request, 0, request.shortest_duration);
  expect_bool("forward from 5.104 finds candidate", selection.found, true);
  if (selection.found) {
    expect_bool("forward from 5.104 is right-owned", selection.candidate.owner == SideType::Right, true);
    expect_near("forward from 5.104 lands shifted right key", selection.candidate.comparison_timeline_position, 5.168F);
  }

  request.unadjusted_static_right_time_shift = 42000;
  const playback_keyframe::UnionKeyframeSelection bumped = playback_keyframe::select_union_keyframe_candidate(
      +1, 5.104F, synthetic_adjacent_ok(5.208333F), synthetic_adjacent_ok(5.208333F), true, true, request, 0, request.shortest_duration);
  expect_bool("bumped +42ms still finds candidate", bumped.found, true);
  if (bumped.found) {
    expect_near("bumped +42ms maps earlier than canonical +40ms", bumped.candidate.comparison_timeline_position, 5.166333F);
  }
}

static void test_shifted_raw_coincident_keyframes_are_distinct() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.time_shift_multiplier = AVRational{1, 1};
  request.unadjusted_static_right_time_shift = 80000;
  request.left_pts = static_cast<int64_t>(4.167 / AV_TIME_TO_SEC);

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      +1, 4.167F, synthetic_adjacent_ok(5.208333F), synthetic_adjacent_ok(5.208333F), true, true, request, 0, request.shortest_duration);
  expect_bool("coincident keys forward found", selection.found, true);
  if (selection.found) {
    expect_near("shifted raw-coincident right key maps earlier", selection.candidate.comparison_timeline_position, 5.128333F);
    expect_bool("shifted raw-coincident candidate is right", selection.candidate.owner == SideType::Right, true);
  }
}

static void test_shift_forward_solve_from_4167() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.time_shift_multiplier = AVRational{1, 1};
  request.unadjusted_static_right_time_shift = 82000;
  request.left_pts = static_cast<int64_t>(4.167 / AV_TIME_TO_SEC);
  const float target = 5.128F;
  const playback_keyframe::SeekRelativeSolveResult solved = playback_keyframe::seek_relative_for_left_demuxer_position(request, target, request.shortest_duration);
  expect_bool("80ms bumped shift forward solve", solved.ok, true);
  expect_near("80ms bumped shift forward lands left", playback_keyframe::left_target_position_for_seek_relative(request, solved.seek_relative), target);
}

static void test_union_backward_keeps_distinct_shifted_right_key() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 5208000;
  request.unadjusted_static_right_time_shift = 40000;
  request.time_shift_multiplier = AVRational{1, 1};

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      -1, 5.208F, synthetic_adjacent_ok(4.166667F), synthetic_adjacent_ok(5.208F), true, true, request, 0, request.shortest_duration);
  expect_bool("union backward with shift finds nearest right key", selection.found, true);
  if (selection.found) {
    expect_bool("union backward right owner", selection.candidate.owner == SideType::Right, true);
    expect_near("union backward right landing", selection.candidate.comparison_timeline_position, 5.168F);
  }
}

static void test_union_keyframe_backward_prefers_left() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;
  request.unadjusted_static_right_time_shift = 0;
  request.time_shift_multiplier = AVRational{1, 1};

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      -1, 2.0F, synthetic_adjacent_ok(1.0F), synthetic_adjacent_ok(1.5F), true, true, request, 0, request.shortest_duration);
  expect_bool("union backward found", selection.found, true);
  if (selection.found) {
    expect_near("union backward nearest", selection.candidate.comparison_timeline_position, 1.5F);
  }
}

static void test_mapped_position_fail_closed_without_right() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.rights.clear();
  const playback_keyframe::MappedPosition right_to_left = playback_keyframe::comparison_timeline_for_right_demuxer_position(request, 0, 1.0F, request.shortest_duration);
  expect_bool("right to comparison fails without right input", right_to_left.ok, false);
  const playback_keyframe::MappedPosition left_to_right = playback_keyframe::right_demuxer_for_comparison_timeline(request, 0, 1.0F, request.shortest_duration);
  expect_bool("comparison to right fails without right input", left_to_right.ok, false);
}

static void test_union_static_right_shift_maps_right_candidate() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;
  request.unadjusted_static_right_time_shift = 40000;
  request.time_shift_multiplier = AVRational{1, 1};

  const float right_key = playback_keyframe::current_right_demuxer_position(request, 0) + 1.0F;
  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      +1, 0.0F, synthetic_adjacent_at_boundary(), synthetic_adjacent_ok(right_key), false, true, request, 0, request.shortest_duration);
  expect_bool("union static shift finds right-only candidate", selection.found, true);
  if (selection.found) {
    const playback_keyframe::MappedPosition mapped =
        playback_keyframe::comparison_timeline_for_right_demuxer_position(request, 0, right_key, request.shortest_duration);
    expect_bool("union static shift mapping ok", mapped.ok, true);
    if (mapped.ok) {
      expect_near("union static shift comparison position", selection.candidate.comparison_timeline_position, mapped.seconds);
    }
  }
}

static void test_union_nonunity_multiplier_maps_right_candidate() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;
  request.unadjusted_static_right_time_shift = 0;

  const float right_key = playback_keyframe::current_right_demuxer_position(request, 0) + 0.5F;
  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      +1, 0.0F, synthetic_adjacent_at_boundary(), synthetic_adjacent_ok(right_key), false, true, request, 0, request.shortest_duration);
  expect_bool("union non-unity finds right candidate", selection.found, true);
  if (selection.found) {
    const playback_keyframe::MappedPosition mapped =
        playback_keyframe::comparison_timeline_for_right_demuxer_position(request, 0, right_key, request.shortest_duration);
    expect_bool("union non-unity mapping ok", mapped.ok, true);
    if (mapped.ok) {
      expect_near("union non-unity comparison position", selection.candidate.comparison_timeline_position, mapped.seconds);
    }
  }
}

static void test_union_mapping_failure_skips_right_candidate() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.rights.clear();

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      +1, 0.0F, synthetic_adjacent_ok(1.0F), synthetic_adjacent_ok(2.0F), true, true, request, 0, request.shortest_duration);
  expect_bool("union mapping failure still finds left", selection.found, true);
  if (selection.found) {
    expect_bool("union mapping failure left owner", selection.candidate.owner == SideType::Left, true);
    expect_near("union mapping failure left landing", selection.candidate.comparison_timeline_position, 1.0F);
  }
}

static void test_union_right_reference_uses_comparison_timeline_with_cursor() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;
  request.unadjusted_static_right_time_shift = 0;
  request.time_shift_multiplier = AVRational{1, 1};

  playback_keyframe::KeyframeNavigationCursor cursor;
  playback_keyframe::commit_keyframe_navigation_cursor(cursor, 0, 2.0F);

  const playback_keyframe::UnionRightReference reference = playback_keyframe::right_reference_for_union_navigation(cursor, 0, request, 2.0F, true, true, 0.0F, request.shortest_duration);
  expect_bool("cursor right reference queries adjacent", reference.query_adjacent, true);
  const float plan_right = playback_keyframe::current_right_demuxer_position(request, 0);
  const playback_keyframe::MappedPosition mapped = playback_keyframe::right_demuxer_for_comparison_timeline(request, 0, 2.0F, request.shortest_duration);
  expect_bool("cursor mapped right ref ok", mapped.ok, true);
  if (mapped.ok) {
    expect_near("cursor right reference from comparison timeline", reference.demuxer_seconds, mapped.seconds);
    expect_bool("cursor right reference not lagging display", std::fabs(reference.demuxer_seconds - 0.0F) > 0.25F, true);
    (void)plan_right;
  }
}

static void test_union_selection_cursor_ahead_of_lagging_right_display() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;
  request.unadjusted_static_right_time_shift = 0;
  request.time_shift_multiplier = AVRational{1, 1};

  playback_keyframe::KeyframeNavigationCursor cursor;
  playback_keyframe::commit_keyframe_navigation_cursor(cursor, 0, 2.0F);

  const playback_keyframe::UnionRightReference right_ref = playback_keyframe::right_reference_for_union_navigation(cursor, 0, request, 2.0F, true, true, 0.0F, request.shortest_duration);
  expect_bool("union selection right ref ok", right_ref.query_adjacent, true);

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      +1, 2.0F, synthetic_adjacent_ok(3.0F), synthetic_adjacent_ok(2.5F), true, true, request, 0, request.shortest_duration);
  expect_bool("union selection found nearer right key", selection.found, true);
  if (selection.found) {
    expect_near("union selection lands nearer right future key", selection.candidate.comparison_timeline_position, 2.5F);
    expect_bool("union selection right owner", selection.candidate.owner == SideType::Right, true);
  }
}

static void test_find_adjacent_forward_before_first_keyframe() {
  playback_keyframe::KeyframeIndexSnapshot snapshot;
  snapshot.seekable = true;
  snapshot.presentation_mapping_verified = true;
  snapshot.time_base = AVRational{1, 25};
  snapshot.video_start_time_seconds = 1.0F;
  snapshot.entries = {
      {25, AVINDEX_KEYFRAME},
      {50, AVINDEX_KEYFRAME},
  };
  snapshot.entries[0].presentation_timestamp = 25;
  snapshot.entries[1].presentation_timestamp = 50;

  const playback_keyframe::AdjacentKeyframe next = playback_keyframe::find_adjacent_keyframe(snapshot, +1, 1.05F);
  expect_status("forward before first indexed key", next.status, playback_keyframe::AdjacentKeyframeStatus::Ok);
  if (next.status == playback_keyframe::AdjacentKeyframeStatus::Ok) {
    expect_near("forward before first lands on first key", next.demuxer_position_seconds, 2.0F);
  }

  const playback_keyframe::AdjacentKeyframe back = playback_keyframe::find_adjacent_keyframe(snapshot, -1, 1.05F);
  expect_status("backward before first indexed key", back.status, playback_keyframe::AdjacentKeyframeStatus::AtBoundary);
}

static void test_union_unindexed_right_fallback_left_only() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 0;
  request.unadjusted_static_right_time_shift = 0;
  request.time_shift_multiplier = AVRational{1, 1};

  const playback_keyframe::UnionKeyframeSelection selection = playback_keyframe::select_union_keyframe_candidate(
      +1, 0.0F, synthetic_adjacent_ok(1.0F), synthetic_adjacent_ok(5.0F), true, false, request, 0, request.shortest_duration);
  expect_bool("union left-only indexed forward", selection.found, true);
  if (selection.found) {
    expect_near("union left-only landing", selection.candidate.comparison_timeline_position, 1.0F);
  }
}

static void test_seek_relative_for_left_demuxer_position() {
  playback_seek::SeekRequest request = sample_seek_request();
  request.left_pts = 400000;
  request.unadjusted_static_right_time_shift = 0;
  request.time_shift_multiplier = AVRational{1, 1};

  const float target = 2.0F;
  const playback_keyframe::SeekRelativeSolveResult solved = playback_keyframe::seek_relative_for_left_demuxer_position(request, target, request.shortest_duration);
  expect_bool("left demuxer inverse solved", solved.ok, true);
  expect_near("left demuxer inverse lands", playback_keyframe::left_target_position_for_seek_relative(request, solved.seek_relative), target);
}

static void test_time_shift_nonunity_inverse_via_bisection() {
  playback_seek::SeekRequest request = sample_seek_request();
  const float current = playback_keyframe::current_right_demuxer_position(request, 0);
  const float target = current + 1.25F;
  const playback_keyframe::SeekRelativeSolveResult solved = playback_keyframe::seek_relative_for_right_demuxer_position(request, 0, target, request.shortest_duration);
  expect_bool("non-unity multiplier inverse solved", solved.ok, true);
  playback_seek::SeekRequest probe = request;
  probe.seek_relative = solved.seek_relative;
  const playback_seek::SeekPlan plan = playback_seek::plan_seek(probe);
  expect_near("non-unity inverse hits right target", plan.rights[0].target_position, target);
}

static void test_unverified_snapshot_unavailable() {
  playback_keyframe::KeyframeIndexSnapshot snapshot;
  snapshot.seekable = true;
  snapshot.presentation_mapping_verified = false;
  snapshot.time_base = AVRational{1, 25};
  snapshot.entries.push_back({0, AVINDEX_KEYFRAME});
  expect_bool("unverified snapshot not navigable", playback_keyframe::snapshot_has_keyframe_index(snapshot), false);
  const playback_keyframe::AdjacentKeyframe adjacent = playback_keyframe::find_adjacent_keyframe(snapshot, +1, 0.0F);
  expect_status("unverified adjacent unavailable", adjacent.status, playback_keyframe::AdjacentKeyframeStatus::Unavailable);
}

static void test_real_variable_gop_keyframes(const std::string& path, const char* label, const std::vector<float>& expect_key_pts) {
  if (path.empty()) {
    return;
  }

  AVFormatContext* format_context = nullptr;
  if (avformat_open_input(&format_context, path.c_str(), nullptr, nullptr) < 0) {
    std::fprintf(stderr, "FAIL could not open %s %s\n", label, path.c_str());
    failures++;
    return;
  }

  if (avformat_find_stream_info(format_context, nullptr) < 0) {
    avformat_close_input(&format_context);
    std::fprintf(stderr, "FAIL could not probe %s %s\n", label, path.c_str());
    failures++;
    return;
  }

  const int video_stream = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  const playback_keyframe::KeyframeIndexSnapshot snapshot = playback_keyframe::capture_verified_keyframe_index_snapshot(format_context, video_stream, 0.0F);
  avformat_close_input(&format_context);

  {
    const std::string mapping_label = std::string(label) + " mapping verified";
    expect_bool(mapping_label.c_str(), snapshot.presentation_mapping_verified, true);
  }
  const std::vector<float> positions = keyframe_demuxer_positions_from_snapshot(snapshot);
  if (positions.size() != expect_key_pts.size()) {
    std::fprintf(stderr, "FAIL %s keyframe count %zu expected %zu\n", label, positions.size(), expect_key_pts.size());
    failures++;
    return;
  }

  for (size_t i = 0; i < positions.size(); ++i) {
    if (std::fabs(positions[i] - expect_key_pts[i]) > kPresentationLatticeTol) {
      std::fprintf(stderr, "FAIL %s keyframe %zu position %.6f expected %.6f\n", label, i, positions[i], expect_key_pts[i]);
      failures++;
    } else {
      std::printf("PASS %s keyframe %zu at %.6f s\n", label, i, positions[i]);
    }
  }

  float cursor = expect_key_pts.front();
  for (size_t i = 1; i < expect_key_pts.size(); ++i) {
    const playback_keyframe::AdjacentKeyframe next = playback_keyframe::find_adjacent_keyframe(snapshot, +1, cursor);
    const std::string step_label = std::string(label) + " forward step";
    expect_status(step_label.c_str(), next.status, playback_keyframe::AdjacentKeyframeStatus::Ok);
    if (next.status == playback_keyframe::AdjacentKeyframeStatus::Ok) {
      const std::string land_label = std::string(label) + " forward landing";
      expect_near(land_label.c_str(), next.demuxer_position_seconds, expect_key_pts[i]);
      cursor = next.demuxer_position_seconds;
    }
  }
}

static void test_start_time_roundtrip() {
  const AVRational time_base{1, 25};
  const float start_time = 1.25F;
  const int64_t stream_ts = 50;
  const float demuxer_pos = playback_keyframe::stream_timestamp_to_demuxer_seconds(stream_ts, time_base, start_time);
  const int64_t roundtrip_ts = playback_keyframe::demuxer_seconds_to_stream_timestamp(demuxer_pos, time_base, start_time);
  expect_i64("nonzero start time stream ts roundtrip", roundtrip_ts, stream_ts);
}

static void test_fixture_index_navigation(const std::string& path) {
  if (path.empty()) {
    std::printf("SKIP fixture index navigation (set KEYFRAME_TEST_FIXTURE or pass path)\n");
    return;
  }

  AVFormatContext* format_context = nullptr;
  if (avformat_open_input(&format_context, path.c_str(), nullptr, nullptr) < 0) {
    std::fprintf(stderr, "FAIL could not open fixture %s\n", path.c_str());
    failures++;
    return;
  }

  if (avformat_find_stream_info(format_context, nullptr) < 0) {
    avformat_close_input(&format_context);
    std::fprintf(stderr, "FAIL could not probe fixture %s\n", path.c_str());
    failures++;
    return;
  }

  const int video_stream = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video_stream < 0) {
    avformat_close_input(&format_context);
    std::fprintf(stderr, "FAIL no video stream in %s\n", path.c_str());
    failures++;
    return;
  }

  const playback_keyframe::KeyframeIndexSnapshot snapshot = playback_keyframe::capture_verified_keyframe_index_snapshot(format_context, video_stream, 0.0F);
  if (!playback_keyframe::snapshot_has_keyframe_index(snapshot)) {
    avformat_close_input(&format_context);
    std::fprintf(stderr, "FAIL fixture has no keyframe index: %s\n", path.c_str());
    failures++;
    return;
  }

  const playback_keyframe::AdjacentKeyframe next_mid = playback_keyframe::find_adjacent_keyframe(snapshot, +1, 1.0F);
  expect_status("fixture next from 1s", next_mid.status, playback_keyframe::AdjacentKeyframeStatus::Ok);
  if (next_mid.status == playback_keyframe::AdjacentKeyframeStatus::Ok) {
    if (!(next_mid.demuxer_position_seconds > 1.0F)) {
      std::fprintf(stderr, "FAIL fixture next keyframe position %.3f did not move forward from 1s\n", next_mid.demuxer_position_seconds);
      failures++;
    } else {
      std::printf("PASS fixture next keyframe at %.3f s\n", next_mid.demuxer_position_seconds);
    }

    const playback_keyframe::AdjacentKeyframe prev_back = playback_keyframe::find_adjacent_keyframe(snapshot, -1, next_mid.demuxer_position_seconds);
    expect_status("fixture prev after forward", prev_back.status, playback_keyframe::AdjacentKeyframeStatus::Ok);

    float first_keyframe_position = 0.0F;
    bool found_first_keyframe = false;
    for (const playback_keyframe::IndexEntry& entry : snapshot.entries) {
      if ((entry.flags & AVINDEX_KEYFRAME) == 0) {
        continue;
      }
      const int64_t presentation_timestamp = playback_keyframe::index_entry_presentation_timestamp(snapshot, entry);
      const float position = playback_keyframe::stream_timestamp_to_demuxer_seconds(presentation_timestamp, snapshot.time_base, snapshot.video_start_time_seconds);
      if (!found_first_keyframe || position < first_keyframe_position) {
        first_keyframe_position = position;
        found_first_keyframe = true;
      }
    }
    if (!found_first_keyframe) {
      std::fprintf(stderr, "FAIL fixture keyframe index contains no keyframes\n");
      failures++;
    } else {
      const playback_keyframe::AdjacentKeyframe prev_start = playback_keyframe::find_adjacent_keyframe(snapshot, -1, first_keyframe_position);
      expect_status("fixture prev at first keyframe boundary", prev_start.status, playback_keyframe::AdjacentKeyframeStatus::AtBoundary);
    }
  }

  // Exercise the actual on-demand probe from positions between keyframes,
  // comparing against a fully probed index. This catches DTS-vs-PTS errors
  // that synthetic index-only tests cannot expose.
  playback_keyframe::KeyframeIndexSnapshot lazy =
      playback_keyframe::capture_keyframe_index_snapshot(format_context, video_stream, 0.0F);
  const std::vector<float> positions = keyframe_demuxer_positions_from_snapshot(snapshot);
  for (size_t i = 1; i < positions.size() && i < 4; ++i) {
    const float reference = 0.5F * (positions[i - 1] + positions[i]);
    for (const int direction : {-1, +1}) {
      const bool prepared = playback_keyframe::prepare_adjacent_keyframe_probe(format_context, video_stream, lazy, direction, reference);
      expect_bool("lazy probe prepared", prepared, true);
      if (!prepared) {
        continue;
      }
      const playback_keyframe::AdjacentKeyframe expected = playback_keyframe::find_adjacent_keyframe(snapshot, direction, reference);
      const playback_keyframe::AdjacentKeyframe actual = playback_keyframe::find_adjacent_keyframe(lazy, direction, reference);
      expect_status("lazy probe adjacent status", actual.status, expected.status);
      if (actual.status == playback_keyframe::AdjacentKeyframeStatus::Ok && expected.status == playback_keyframe::AdjacentKeyframeStatus::Ok) {
        expect_near("lazy probe adjacent PTS", actual.demuxer_position_seconds, expected.demuxer_position_seconds);
      }
    }
  }

  avformat_close_input(&format_context);
}

static bool demuxer_style_seek(AVFormatContext* format_context, const float position, const bool backward) {
  const int64_t seek_target = static_cast<int64_t>(position * AV_TIME_BASE);
  return av_seek_frame(format_context, -1, seek_target, backward ? AVSEEK_FLAG_BACKWARD : 0) >= 0;
}

static float first_keyframe_demuxer_position(AVFormatContext* format_context, const int video_stream, const AVRational& time_base, const float video_start_time_seconds) {
  avformat_flush(format_context);

  AVPacket packet{};

  for (int packets = 0; packets < 256; ++packets) {
    if (av_read_frame(format_context, &packet) < 0) {
      break;
    }

    const bool ours = packet.stream_index == video_stream;
    const bool key = (packet.flags & AV_PKT_FLAG_KEY) != 0;

    if (!ours || !key || packet.pts == AV_NOPTS_VALUE) {
      av_packet_unref(&packet);
      continue;
    }

    const float position = playback_keyframe::stream_timestamp_to_demuxer_seconds(packet.pts, time_base, video_start_time_seconds);
    av_packet_unref(&packet);
    return position;
  }

  return std::numeric_limits<float>::quiet_NaN();
}

static void test_real_media_demuxer_keyframe_seek(const std::string& path, const char* label, const std::vector<float>& backward_targets, const std::vector<float>& expected_landings) {
  if (path.empty() || backward_targets.size() != expected_landings.size()) {
    return;
  }

  AVFormatContext* format_context = nullptr;
  if (avformat_open_input(&format_context, path.c_str(), nullptr, nullptr) < 0) {
    std::fprintf(stderr, "FAIL could not open %s %s\n", label, path.c_str());
    failures++;
    return;
  }

  if (avformat_find_stream_info(format_context, nullptr) < 0) {
    avformat_close_input(&format_context);
    std::fprintf(stderr, "FAIL could not probe %s %s\n", label, path.c_str());
    failures++;
    return;
  }

  const int video_stream = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  const playback_keyframe::KeyframeIndexSnapshot snapshot = playback_keyframe::capture_verified_keyframe_index_snapshot(format_context, video_stream, 0.0F);

  constexpr float kKeyframeSnapBias = 1e-3F;

  for (size_t i = 0; i < backward_targets.size(); ++i) {
    if (demuxer_style_seek(format_context, 0.0F, true)) {
      avformat_flush(format_context);
    }

    const float target = backward_targets[i];
    const float seek_position = target + kKeyframeSnapBias;

    if (!demuxer_style_seek(format_context, seek_position, true)) {
      std::fprintf(stderr, "FAIL %s backward seek to %.6f\n", label, static_cast<double>(seek_position));
      failures++;
      continue;
    }

    const float landed = first_keyframe_demuxer_position(format_context, video_stream, snapshot.time_base, snapshot.video_start_time_seconds);
    const std::string step_label = std::string(label) + " backward keyframe land";
    expect_near(step_label.c_str(), landed, expected_landings[i]);
  }

  avformat_close_input(&format_context);
}

static void test_real_hevc_forward_seek_overshoots(const std::string& path) {
  if (path.empty()) {
    return;
  }

  AVFormatContext* format_context = nullptr;
  if (avformat_open_input(&format_context, path.c_str(), nullptr, nullptr) < 0) {
    return;
  }

  if (avformat_find_stream_info(format_context, nullptr) < 0) {
    avformat_close_input(&format_context);
    return;
  }

  const int video_stream = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  const playback_keyframe::KeyframeIndexSnapshot snapshot = playback_keyframe::capture_verified_keyframe_index_snapshot(format_context, video_stream, 0.0F);

  const float target = 1.041667F;
  const float overshoot_expected = 2.083333F;

  if (demuxer_style_seek(format_context, target, false)) {
    const float landed = first_keyframe_demuxer_position(format_context, video_stream, snapshot.time_base, snapshot.video_start_time_seconds);
    expect_near("real-hevc forward seek without BACKWARD overshoots", landed, overshoot_expected);
  }

  avformat_close_input(&format_context);
}

int main(int argc, char** argv) {
  if (!playback_keyframe::libavformat_index_api_available()) {
    std::printf("SKIP playback_keyframe tests (libavformat index API unavailable)\n");
    return EXIT_SUCCESS;
  }

  test_start_time_roundtrip();
  test_synthetic_reference_index_ignores_non_keyframes();
  test_keyframe_navigation_cursor_and_reference();
  test_keyframe_navigation_cursor_walk_sparse_left();
  test_synthetic_pts_dts_mapping_navigation();
  test_unverified_snapshot_unavailable();
  test_nonuniform_mapping_rejects_unprobed_pts();
  test_nonuniform_different_dts_pts_offsets();
  test_time_shift_nonunity_inverse_via_bisection();
  test_union_keyframe_prefers_nearest_forward();
  test_union_keyframe_coincident_dedupes();
  test_union_keyframe_respects_left_start_offset();
  test_union_keyframe_no_next_at_boundary();
  test_union_right_adjacent_reference_skips_current_key();
  test_union_sparse_left_successive_right_owned_steps();
  test_union_forward_from_common_5104_with_40ms_shift();
  test_shifted_raw_coincident_keyframes_are_distinct();
  test_shift_forward_solve_from_4167();
  test_union_backward_keeps_distinct_shifted_right_key();
  test_union_keyframe_backward_prefers_left();
  test_mapped_position_fail_closed_without_right();
  test_union_static_right_shift_maps_right_candidate();
  test_union_nonunity_multiplier_maps_right_candidate();
  test_union_mapping_failure_skips_right_candidate();
  test_union_right_reference_uses_comparison_timeline_with_cursor();
  test_union_selection_cursor_ahead_of_lagging_right_display();
  test_find_adjacent_forward_before_first_keyframe();
  test_union_unindexed_right_fallback_left_only();
  test_union_empty_right_unmapped_left_boundary_is_unavailable();
  test_union_backward_shifted_right_before_start_classifications();
  test_union_forward_right_unmapped_stays_unavailable();
  test_union_backward_right_unmapped_no_rights_stays_unavailable();
  test_union_backward_reachable_right_key_still_selected();
  test_union_backward_left_maps_before_negative_shifted_right_start();
  test_union_backward_negative_shift_reachable_left_still_selected();
  test_union_empty_right_unavailable_left_boundary_is_unavailable();
  test_union_empty_both_at_boundary();
  test_union_left_candidate_right_unavailable_query();
  test_left_single_frame_inverse_unsupported();
  test_left_inverse_clamp_first_pts();
  test_seek_relative_for_left_demuxer_position();

  {
    const playback_seek::SeekRequest request = sample_seek_request();
    const float current_right = playback_keyframe::current_right_demuxer_position(request, 0);
    const float target = current_right + 0.75F;
    const playback_keyframe::SeekRelativeSolveResult solved = playback_keyframe::seek_relative_for_right_demuxer_position(request, 0, target, request.shortest_duration);
    expect_bool("inverse seek solved", solved.ok, true);

    playback_seek::SeekRequest forward = request;
    forward.seek_relative = solved.seek_relative;
    const playback_seek::SeekPlan plan = playback_seek::plan_seek(forward);
    expect_near("inverse seek hits right target", plan.rights[0].target_position, target);
  }

  {
    const playback_seek::SeekRequest request = sample_seek_request();
    const float current_right = playback_keyframe::current_right_demuxer_position(request, 0);
    const playback_keyframe::SeekRelativeSolveResult solved = playback_keyframe::seek_relative_for_right_demuxer_position(request, 0, current_right, request.shortest_duration);
    expect_bool("zero delta solved", solved.ok, true);
    playback_seek::SeekRequest probe = request;
    probe.seek_relative = solved.seek_relative;
    const playback_seek::SeekPlan plan = playback_seek::plan_seek(probe);
    expect_near("zero delta stays at current right", plan.rights[0].target_position, current_right);
  }

  expect_i("demuxer_positions_near identical", playback_keyframe::demuxer_positions_near(1.0F, 1.0005F) ? 1 : 0, 1);
  expect_i("demuxer_positions_near different", playback_keyframe::demuxer_positions_near(1.0F, 1.01F) ? 1 : 0, 0);

  test_fixture_index_navigation(fixture_path(argc, argv));

  {
    const char* h264 = std::getenv("KEYFRAME_REAL_H264");
    const char* hevc = std::getenv("KEYFRAME_REAL_HEVC");
    if (h264 != nullptr) {
      const float h264_keys[] = {0.0F, 5.208333F};
      test_real_variable_gop_keyframes(h264, "real-h264", {h264_keys[0], h264_keys[1]});
      test_real_media_demuxer_keyframe_seek(h264, "real-h264-demuxer", {5.208333F}, {5.208333F});
      test_real_media_demuxer_keyframe_seek(h264, "real-h264-demuxer-sparse", {1.041667F}, {0.0F});
    }
    if (hevc != nullptr) {
      AVFormatContext* format_context = nullptr;
      if (avformat_open_input(&format_context, hevc, nullptr, nullptr) >= 0 && avformat_find_stream_info(format_context, nullptr) >= 0) {
        const int video_stream = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        const playback_keyframe::KeyframeIndexSnapshot hevc_snapshot =
            playback_keyframe::capture_verified_keyframe_index_snapshot(format_context, video_stream, 0.0F);
        avformat_close_input(&format_context);
        expect_bool("real-hevc mapping verified", hevc_snapshot.presentation_mapping_verified, true);
        int64_t first_key_offset = 0;
        int64_t second_key_offset = 0;
        int keyframes_seen = 0;
        for (const playback_keyframe::IndexEntry& entry : hevc_snapshot.entries) {
          if ((entry.flags & AVINDEX_KEYFRAME) == 0 || entry.presentation_timestamp == AV_NOPTS_VALUE) {
            continue;
          }
          const int64_t offset = entry.presentation_timestamp - entry.timestamp;
          if (keyframes_seen == 0) {
            first_key_offset = offset;
          } else if (keyframes_seen == 1) {
            second_key_offset = offset;
            break;
          }
          keyframes_seen++;
        }
        if (keyframes_seen >= 2) {
          expect_i("real-hevc variable dts->pts offsets", first_key_offset != second_key_offset ? 1 : 0, 1);
        }
      }

      const std::vector<float> hevc_keys = {0.0F,        1.041667F,   2.083333F,   3.125000F,   4.166667F,
                                          5.208333F,   6.250000F,   7.291667F,   8.333333F,   9.375000F};
      test_real_variable_gop_keyframes(hevc, "real-hevc", hevc_keys);
      test_real_hevc_forward_seek_overshoots(hevc);
      test_real_media_demuxer_keyframe_seek(hevc, "real-hevc-demuxer", {1.041667F, 2.083333F, 5.208333F}, {1.041667F, 2.083333F, 5.208333F});
    }
  }

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }

  std::printf("All playback keyframe tests passed\n");
  return EXIT_SUCCESS;
}
