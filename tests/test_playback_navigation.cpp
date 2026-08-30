#include "playback_navigation.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

static int failures = 0;

static constexpr float kSeekTol = 1e-5F;

static void expect_i(const char* label, const int got, const int expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: expected %d, got %d\n", label, expected, got);
    failures++;
    return;
  }
  std::printf("PASS %s -> %d\n", label, got);
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
  if (!(std::fabs(got - expected) <= kSeekTol)) {
    std::fprintf(stderr, "FAIL %s: expected %.8g, got %.8g\n", label, static_cast<double>(expected), static_cast<double>(got));
    failures++;
    return;
  }
  std::printf("PASS %s -> %.8g\n", label, static_cast<double>(got));
}

static void expect_ping_pong(const char* label, const int offset, const int last, const bool forward, const int expected_offset, const bool expected_forward) {
  const playback_navigation::PingPongStep step = playback_navigation::next_ping_pong_step(offset, last, forward);
  if (step.offset != expected_offset || step.forward != expected_forward) {
    std::fprintf(stderr, "FAIL %s: expected offset=%d forward=%d, got offset=%d forward=%d\n", label, expected_offset, static_cast<int>(expected_forward), step.offset, static_cast<int>(step.forward));
    failures++;
    return;
  }
  std::printf("PASS %s -> offset=%d forward=%s\n", label, step.offset, step.forward ? "true" : "false");
}

int main() {
  using playback_navigation::adjusted_frame_offset;
  using playback_navigation::next_forward_only_offset;
  using playback_navigation::seek_relative_after_backward_navigation;

  expect_near("nav -1 of 40000 us", seek_relative_after_backward_navigation(0.0F, -1, 40000), -0.04F);
  expect_near("nav -2 of 33333 us from 0.25", seek_relative_after_backward_navigation(0.25F, -2, 33333), 0.183334F);
  expect_near("nav 0 leaves seek_relative", seek_relative_after_backward_navigation(0.25F, 0, 40000), 0.25F);

  expect_i("forward-only last=5 offset=0 wraps", next_forward_only_offset(0, 5), 5);
  expect_i("forward-only last=5 offset=5 steps", next_forward_only_offset(5, 5), 4);
  expect_i("forward-only last=5 offset=3 steps", next_forward_only_offset(3, 5), 2);
  expect_i("forward-only last=0 offset=0 stays", next_forward_only_offset(0, 0), 0);
  expect_i("forward-only last=1 offset=0 wraps", next_forward_only_offset(0, 1), 1);
  expect_i("forward-only last=1 offset=1 steps", next_forward_only_offset(1, 1), 0);

  expect_ping_pong("pingpong last=0 forward", 0, 0, true, 0, true);
  expect_ping_pong("pingpong last=0 reverse", 0, 0, false, 0, false);

  expect_ping_pong("pingpong last=1 offset=0 forward", 0, 1, true, 1, false);
  expect_ping_pong("pingpong last=1 offset=0 reverse", 0, 1, false, 0, true);
  expect_ping_pong("pingpong last=1 offset=1 forward", 1, 1, true, 1, false);
  expect_ping_pong("pingpong last=1 offset=1 reverse", 1, 1, false, 0, true);

  expect_ping_pong("pingpong last=4 offset=0 forward", 0, 4, true, 1, false);
  expect_ping_pong("pingpong last=4 offset=0 reverse", 0, 4, false, 0, true);
  expect_ping_pong("pingpong last=4 offset=4 forward", 4, 4, true, 4, false);
  expect_ping_pong("pingpong last=4 offset=4 reverse", 4, 4, false, 3, true);
  expect_ping_pong("pingpong last=4 offset=2 forward", 2, 4, true, 1, true);
  expect_ping_pong("pingpong last=4 offset=2 reverse", 2, 4, false, 3, false);

  expect_i("offset mid +1", adjusted_frame_offset(2, 1, 5), 3);
  expect_i("offset mid -1", adjusted_frame_offset(2, -1, 5), 1);
  expect_i("offset clamp below zero", adjusted_frame_offset(0, -1, 5), 0);
  expect_i("offset clamp above last", adjusted_frame_offset(5, 1, 5), 5);
  expect_i("offset last=0", adjusted_frame_offset(0, 1, 0), 0);
  expect_i("offset empty common buffer becomes -1", adjusted_frame_offset(0, 0, -1), -1);

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }

  std::printf("All playback navigation tests passed\n");
  return EXIT_SUCCESS;
}
