#include "playback_timing.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

static int failures = 0;

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

int main() {
  using playback_timing::calculate_dynamic_time_shift;
  using playback_timing::compute_frame_delay;
  using playback_timing::compute_min_delta;
  using playback_timing::is_behind;
  using playback_timing::is_in_sync;
  using playback_timing::normalized_delta;
  using playback_timing::time_ms_to_av_time;

  // is_behind converts PTS to float seconds. Tolerance is max(delta_s - 1e-5F, 1/480 s)
  // with a strict <. Exact 1 us threshold cases are omitted: builds use -Ofast on
  // multiple toolchains.

  expect_i64("min delta equal 25fps", compute_min_delta(40000, 40000), 32000);
  expect_i64("min delta truncation", compute_min_delta(40000, 33333), 26666);
  expect_i64("min delta both zero", compute_min_delta(0, 0), 0);

  expect_bool("equal PTS is not behind", is_behind(1000000, 1000000, 32000), false);
  expect_bool("31000 us behind stays in tolerance", is_behind(1000000, 1031000, 32000), false);
  expect_bool("40000 us behind is behind", is_behind(1000000, 1040000, 32000), true);
  expect_bool("ahead is not behind", is_behind(1040000, 1000000, 32000), false);

  expect_bool("1/480 floor 2ms", is_behind(1000000, 1002000, 0), false);
  expect_bool("1/480 floor 3ms", is_behind(1000000, 1003000, 0), true);

  expect_bool("in sync 31000 us", is_in_sync(0, 31000, 40000, 40000), true);
  expect_bool("out of sync 40000 us", is_in_sync(0, 40000, 40000, 40000), false);
  expect_bool("out of sync 40000 us swapped", is_in_sync(40000, 0, 40000, 40000), false);

  expect_i64("frame delay prefers larger", compute_frame_delay(40000, 33333), 40000);
  expect_i64("frame delay reversed", compute_frame_delay(33333, 40000), 40000);

  expect_i64("normalized positive", normalized_delta(40000), 40000);
  expect_i64("normalized zero", normalized_delta(0), 10000);
  expect_i64("normalized negative", normalized_delta(-1), 10000);

  expect_i64("2.5 ms to AV time", time_ms_to_av_time(2.5), 2500);
  expect_i64("-2.5 ms to AV time", time_ms_to_av_time(-2.5), -2500);
  expect_i64("0.5 ms to AV time", time_ms_to_av_time(0.5), 500);

  expect_i64("unity multiplier", calculate_dynamic_time_shift(AVRational{1, 1}, 1000000, false), 0);
  expect_i64("forward 1001/1000", calculate_dynamic_time_shift(AVRational{1001, 1000}, 1000000, false), 1000);
  expect_i64("inverse 1001/1000", calculate_dynamic_time_shift(AVRational{1001, 1000}, 1000000, true), 999);

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }

  std::printf("All playback timing tests passed\n");
  return EXIT_SUCCESS;
}
