#include "../frame_metadata.h"
#include <cstdio>
#include <cstdlib>

static int failures = 0;

static void expect_dar(const char* label, int width, int height, AVRational sar, int expected_num, int expected_den) {
  AVRational dar{};
  if (!FrameMetadata::display_aspect_ratio(width, height, sar, &dar)) {
    std::fprintf(stderr, "FAIL %s: expected %d:%d, got unavailable\n", label, expected_num, expected_den);
    failures++;
    return;
  }
  if (av_cmp_q(dar, AVRational{expected_num, expected_den}) != 0) {
    std::fprintf(stderr, "FAIL %s: expected %d:%d, got %d:%d\n", label, expected_num, expected_den, dar.num, dar.den);
    failures++;
    return;
  }
  std::printf("PASS %s -> %d:%d\n", label, dar.num, dar.den);
}

static void expect_unavailable(const char* label, int width, int height, AVRational sar) {
  AVRational dar{};
  if (FrameMetadata::display_aspect_ratio(width, height, sar, &dar)) {
    std::fprintf(stderr, "FAIL %s: expected unavailable, got %d:%d\n", label, dar.num, dar.den);
    failures++;
    return;
  }
  std::printf("PASS %s -> unavailable\n", label);
}

int main() {
  expect_dar("720x480 SAR 8:9", 720, 480, AVRational{8, 9}, 4, 3);
  expect_dar("720x480 SAR 1:1", 720, 480, AVRational{1, 1}, 3, 2);
  expect_dar("1920x1080 SAR 1:1", 1920, 1080, AVRational{1, 1}, 16, 9);
  expect_dar("1280x720 SAR 1:1", 1280, 720, AVRational{1, 1}, 16, 9);
  expect_dar("1440x1080 SAR 4:3", 1440, 1080, AVRational{4, 3}, 16, 9);
  expect_dar("720x480 SAR 0:1", 720, 480, AVRational{0, 1}, 3, 2);
  expect_unavailable("zero width", 0, 480, AVRational{1, 1});
  expect_unavailable("negative height", 720, -480, AVRational{1, 1});

  AVRational hd{};
  AVRational sd{};
  if (!FrameMetadata::display_aspect_ratio(1920, 1080, AVRational{1, 1}, &hd) || !FrameMetadata::display_aspect_ratio(1280, 720, AVRational{1, 1}, &sd) || av_cmp_q(hd, sd) != 0) {
    std::fprintf(stderr, "FAIL 1920x1080 and 1280x720 should share 16:9\n");
    failures++;
  } else {
    std::printf("PASS 1920x1080 and 1280x720 compare equal\n");
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d test(s) failed\n", failures);
    return EXIT_FAILURE;
  }

  std::printf("All tests passed\n");
  return EXIT_SUCCESS;
}
