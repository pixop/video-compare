#include "playback_seek.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

static int failures = 0;

static constexpr float kPosTol = 1e-5F;

static playback_seek::SeekRequest base_request() {
  playback_seek::SeekRequest request;
  request.seek_relative = 0.04F;
  request.seek_from_start = false;
  request.force_seek_current_position = false;
  request.all_sides_multi_frame = true;
  request.shift_right_frames = 0;
  request.shortest_duration = 10.0;
  request.left_pts = 1000000;
  request.unadjusted_static_right_time_shift = 0;
  request.time_shift_multiplier = AVRational{1, 1};
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

static void expect_i64(const char* label, const int64_t got, const int64_t expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: expected %lld, got %lld\n", label, static_cast<long long>(expected), static_cast<long long>(got));
    failures++;
    return;
  }
  std::printf("PASS %s -> %lld\n", label, static_cast<long long>(got));
}

static void expect_bool(const char* label, const bool got, const bool expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: expected %d, got %d\n", label, static_cast<int>(expected), static_cast<int>(got));
    failures++;
    return;
  }
  std::printf("PASS %s -> %s\n", label, got ? "true" : "false");
}

static void expect_near(const char* label, const float got, const float expected) {
  if (!(std::fabs(got - expected) <= kPosTol)) {
    std::fprintf(stderr, "FAIL %s: expected %.8g, got %.8g\n", label, static_cast<double>(expected), static_cast<double>(got));
    failures++;
    return;
  }
  std::printf("PASS %s -> %.8g\n", label, static_cast<double>(got));
}

static float left_restore(const playback_seek::SeekRequest& request) {
  return request.left_pts * AV_TIME_TO_SEC + request.left.start_time;
}

static float right_restore(const playback_seek::SeekRequest& request, const size_t index) {
  return request.left_pts * AV_TIME_TO_SEC + request.rights[index].start_time;
}

int main() {
  using playback_seek::compute_post_seek_static_right_time_shift;
  using playback_seek::plan_seek;

  {
    const playback_seek::SeekRequest request = base_request();
    const playback_seek::SeekPlan plan = plan_seek(request);
    expect_near("relative left target", plan.left_target_position, left_restore(request) + 0.04F);
    expect_near("relative left restore", plan.left_restore_position, left_restore(request));
    expect_near("relative right target", plan.rights[0].target_position, right_restore(request, 0) + 0.04F);
    expect_near("relative right restore", plan.rights[0].restore_position, right_restore(request, 0));
    expect_bool("relative seek is forward", plan.backward, false);
  }

  {
    playback_seek::SeekRequest request = base_request();
    request.seek_relative = -0.04F;
    const playback_seek::SeekPlan plan = plan_seek(request);
    expect_bool("negative relative seek is backward", plan.backward, true);
    expect_near("backward left target", plan.left_target_position, left_restore(request) - 0.04F);
    expect_near("backward right target", plan.rights[0].target_position, right_restore(request, 0) - 0.04F);
  }

  {
    playback_seek::SeekRequest request = base_request();
    request.seek_relative = 0.0F;
    request.shift_right_frames = 1;
    const playback_seek::SeekPlan plan = plan_seek(request);
    expect_bool("shift-right forces backward", plan.backward, true);
    expect_near("shift-right left stays current", plan.left_target_position, left_restore(request));
  }

  {
    playback_seek::SeekRequest request = base_request();
    request.seek_relative = 0.0F;
    request.force_seek_current_position = true;
    request.all_sides_multi_frame = true;
    expect_bool("force-current all multi-frame is backward", plan_seek(request).backward, true);

    request.all_sides_multi_frame = false;
    expect_bool("force-current without all multi-frame is forward", plan_seek(request).backward, false);
  }

  {
    playback_seek::SeekRequest request = base_request();
    request.seek_from_start = true;
    request.seek_relative = 0.5F;
    request.left.single_frame = true;
    request.left.delta_pts = 40000;
    const playback_seek::SeekPlan plan = plan_seek(request);
    const float rewritten = static_cast<float>(40000 * AV_TIME_TO_SEC);

    expect_near("left still ignores timeline fraction", plan.left_target_position, left_restore(request) + rewritten);
    expect_bool("rewritten still-left seek is forward", plan.backward, false);
    expect_near("from-start right uses rewritten seek_relative", plan.rights[0].target_position, request.shortest_duration * rewritten + request.rights[0].start_time);
  }

  {
    playback_seek::SeekRequest request = base_request();
    request.seek_from_start = false;
    request.seek_relative = -0.5F;
    request.left.single_frame = true;
    request.left.delta_pts = 40000;
    const playback_seek::SeekPlan plan = plan_seek(request);
    const float rewritten = static_cast<float>(40000 * AV_TIME_TO_SEC);

    expect_near("relative left still rewrites seek", plan.left_target_position, left_restore(request) + rewritten);
    expect_bool("relative left still rewrite changes backward", plan.backward, false);
    expect_near("relative right sees left still rewrite", plan.rights[0].target_position, right_restore(request, 0) + rewritten);
  }

  {
    playback_seek::SeekRequest request = base_request();
    request.seek_from_start = false;
    request.seek_relative = -0.5F;
    request.rights[0].single_frame = true;
    request.rights[0].delta_pts = 40000;
    const playback_seek::SeekPlan plan = plan_seek(request);
    const float rewritten = static_cast<float>(40000 * AV_TIME_TO_SEC);

    expect_bool("relative right still does not change backward", plan.backward, true);
    expect_near("relative right still rewrites seek", plan.rights[0].target_position, right_restore(request, 0) + rewritten);
  }

  {
    playback_seek::SeekRequest request = base_request();
    request.seek_from_start = true;
    request.seek_relative = 0.5F;
    request.rights.clear();

    playback_seek::SeekSideInput right0;
    right0.start_time = 0.0F;
    right0.first_pts = 0;
    right0.delta_pts = 40000;
    right0.single_frame = true;

    playback_seek::SeekSideInput right1;
    right1.start_time = 1.5F;
    right1.first_pts = 0;
    right1.delta_pts = 33333;
    right1.single_frame = false;

    playback_seek::SeekSideInput right2;
    right2.start_time = 2.0F;
    right2.first_pts = 0;
    right2.delta_pts = 20000;
    right2.single_frame = false;

    request.rights.push_back(right0);
    request.rights.push_back(right1);
    request.rights.push_back(right2);

    const playback_seek::SeekPlan plan = plan_seek(request);
    const float rewritten = static_cast<float>(40000 * AV_TIME_TO_SEC);

    expect_near("left from-start keeps 0.5", plan.left_target_position, request.shortest_duration * 0.5F + request.left.start_time);
    expect_bool("backward stays from pre-right seek_relative", plan.backward, false);
    expect_near("right0 still uses assigned delta", plan.rights[0].target_position, right_restore(request, 0) + rewritten);
    expect_near("right1 from-start sees right0 rewrite", plan.rights[1].target_position, request.shortest_duration * rewritten + right1.start_time);
    expect_near("right2 from-start sees same rewrite", plan.rights[2].target_position, request.shortest_duration * rewritten + right2.start_time);
    expect_near("right1 restore uses left pts + own start", plan.rights[1].restore_position, request.left_pts * AV_TIME_TO_SEC + 1.5F);
    expect_near("right2 restore uses left pts + own start", plan.rights[2].restore_position, request.left_pts * AV_TIME_TO_SEC + 2.0F);
  }

  {
    playback_seek::SeekRequest request = base_request();
    request.seek_from_start = true;
    request.seek_relative = -0.5F;
    request.rights[0].single_frame = true;
    request.rights[0].delta_pts = 40000;
    const playback_seek::SeekPlan plan = plan_seek(request);

    expect_bool("right still-frame rewrite does not flip backward", plan.backward, true);
    expect_near("left from-start clamp after negative 0.5", plan.left_target_position, 0.0F);
  }

  {
    playback_seek::SeekRequest request = base_request();
    request.seek_relative = -2.0F;
    request.left.first_pts = 500000;
    request.rights[0].first_pts = 250000;
    const playback_seek::SeekPlan plan = plan_seek(request);
    expect_near("left clamps to known first pts", plan.left_target_position, 500000 * AV_TIME_TO_SEC + request.left.start_time);
    expect_near("right clamps to known first pts before shift", plan.rights[0].target_position, 250000 * AV_TIME_TO_SEC + request.rights[0].start_time);
  }

  {
    playback_seek::SeekRequest request = base_request();
    request.seek_relative = -2.0F;
    request.left.first_pts = std::numeric_limits<int64_t>::min();
    request.left.start_time = 0.25F;
    request.rights[0].first_pts = std::numeric_limits<int64_t>::min();
    request.rights[0].start_time = 0.5F;
    const playback_seek::SeekPlan plan = plan_seek(request);
    expect_near("unknown first pts clamps left to start", plan.left_target_position, 0.25F);
    expect_near("unknown first pts clamps right to start", plan.rights[0].target_position, 0.5F);
  }

  {
    playback_seek::SeekRequest request = base_request();
    request.seek_relative = -2.0F;
    request.unadjusted_static_right_time_shift = -150000;
    const playback_seek::SeekPlan plan = plan_seek(request);
    const float min_right = 0.0F;
    const float expected = min_right + static_cast<float>(-150000 * AV_TIME_TO_SEC);
    expect_near("negative static shift can pass below min", plan.rights[0].target_position, expected);
    if (plan.rights[0].target_position >= min_right) {
      std::fprintf(stderr, "FAIL negative static shift should remain below min, got %.8g\n", static_cast<double>(plan.rights[0].target_position));
      failures++;
    } else {
      std::printf("PASS negative static shift remains below min\n");
    }
  }

  {
    playback_seek::SeekRequest request = base_request();
    request.seek_relative = 0.0F;
    request.unadjusted_static_right_time_shift = 150000;
    request.time_shift_multiplier = AVRational{1001, 1000};
    const playback_seek::SeekPlan plan = plan_seek(request);

    float after_static = right_restore(request, 0);
    after_static += 150000 * AV_TIME_TO_SEC;
    const float expected = after_static + static_cast<float>(playback_timing::calculate_dynamic_time_shift(request.time_shift_multiplier, (after_static - request.rights[0].start_time) / AV_TIME_TO_SEC, false)) * AV_TIME_TO_SEC;

    float wrong_order = right_restore(request, 0);
    wrong_order += static_cast<float>(playback_timing::calculate_dynamic_time_shift(request.time_shift_multiplier, (wrong_order - request.rights[0].start_time) / AV_TIME_TO_SEC, false)) * AV_TIME_TO_SEC;
    wrong_order += 150000 * AV_TIME_TO_SEC;

    expect_near("dynamic shift uses already-static-shifted position", plan.rights[0].target_position, expected);
    if (!(std::fabs(expected - wrong_order) > kPosTol)) {
      std::fprintf(stderr, "FAIL dynamic-after-static fixture is not order-sensitive\n");
      failures++;
    } else if (!(std::fabs(plan.rights[0].target_position - wrong_order) > kPosTol)) {
      std::fprintf(stderr, "FAIL right target matched dynamic-before-static order\n");
      failures++;
    } else {
      std::printf("PASS dynamic-after-static differs from reversed order\n");
    }
  }

  {
    playback_seek::SeekRequest request = base_request();
    request.left_pts = 2500000;
    request.rights[0].start_time = 1.25F;
    const playback_seek::SeekPlan plan = plan_seek(request);
    expect_near("right restore is left-master", plan.rights[0].restore_position, 2500000 * AV_TIME_TO_SEC + 1.25F);
    expect_near("right relative base is left-master", plan.rights[0].target_position, 2500000 * AV_TIME_TO_SEC + 1.25F + 0.04F);
  }

  expect_i64("bump 0", compute_post_seek_static_right_time_shift(0), 0);
  expect_i64("bump +500", compute_post_seek_static_right_time_shift(500), 2000);
  expect_i64("bump +1500", compute_post_seek_static_right_time_shift(1500), 3000);
  expect_i64("bump +2000", compute_post_seek_static_right_time_shift(2000), 4000);
  expect_i64("bump -500", compute_post_seek_static_right_time_shift(-500), -2000);
  expect_i64("bump -1500", compute_post_seek_static_right_time_shift(-1500), -3000);
  expect_i64("bump -2000", compute_post_seek_static_right_time_shift(-2000), -4000);
  expect_i64("bump +1", compute_post_seek_static_right_time_shift(1), 2000);
  expect_i64("bump +1000", compute_post_seek_static_right_time_shift(1000), 3000);
  expect_i64("bump -1", compute_post_seek_static_right_time_shift(-1), -2000);
  expect_i64("bump -1000", compute_post_seek_static_right_time_shift(-1000), -3000);

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }

  std::printf("All playback seek tests passed\n");
  return EXIT_SUCCESS;
}
