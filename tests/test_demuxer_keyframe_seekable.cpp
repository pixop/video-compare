#ifdef _WIN32

#include <cstdio>
#include <cstdlib>

int main() {
  std::printf("SKIP demuxer keyframe seekable tests (requires POSIX FIFO)\n");
  return EXIT_SUCCESS;
}

#else

#include "core_types.h"
#include "demuxer.h"
#include "keyframe_navigation.h"
#include "playback_seek.h"

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;

static void expect_bool(const char* label, const bool got, const bool expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: expected %d, got %d\n", label, static_cast<int>(expected), static_cast<int>(got));
    failures++;
    return;
  }
  std::printf("PASS %s -> %s\n", label, got ? "true" : "false");
}

static void expect_status(const char* label, const playback_keyframe::KeyframeNavigationStatus got, const playback_keyframe::KeyframeNavigationStatus expected) {
  if (got != expected) {
    std::fprintf(stderr, "FAIL %s: unexpected status %d\n", label, static_cast<int>(got));
    failures++;
    return;
  }
  std::printf("PASS %s status\n", label);
}

static std::string fixture_path(int argc, char** argv) {
  if (argc >= 2 && argv[1][0] != '\0') {
    return argv[1];
  }
  const char* env = std::getenv("KEYFRAME_TEST_FIXTURE");
  return env != nullptr ? env : "";
}

static playback_seek::SeekRequest sample_seek_request() {
  playback_seek::SeekRequest request;
  request.shortest_duration = 10.0F;
  request.left.start_time = 0.0F;
  request.left.first_pts = 0;
  request.left.delta_pts = 40000;
  request.left.single_frame = false;
  request.unadjusted_static_right_time_shift = 0;
  request.time_shift_multiplier = AVRational{1, 1};

  playback_seek::SeekSideInput right;
  right.start_time = 0.0F;
  right.first_pts = 0;
  right.delta_pts = 40000;
  right.single_frame = false;
  request.rights.push_back(right);
  return request;
}

static pid_t start_pipe_feeder(const std::string& source, const std::string& fifo) {
  const pid_t pid = fork();
  if (pid == 0) {
    execlp("ffmpeg", "ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", source.c_str(), "-c", "copy", "-f", "mpegts", fifo.c_str(), nullptr);
    _exit(127);
  }
  return pid;
}

static void stop_child(const pid_t pid) {
  if (pid <= 0) {
    return;
  }
  kill(pid, SIGTERM);
  int status = 0;
  (void)waitpid(pid, &status, 0);
}

static void test_file_seekable_and_index_gate(const std::string& path) {
  Demuxer demuxer(Side::Left(), "", path, nullptr, nullptr);
  expect_bool("indexed file keyframe_navigation_seekable", demuxer.keyframe_navigation_seekable(), true);
  expect_bool("indexed file index_navigation_supported", demuxer.index_navigation_supported(), true);
}

static void test_ffv1_matroska_navigation(const std::string& path) {
  Demuxer demuxer(Side::Left(), "", path, nullptr, nullptr);
  expect_bool("FFV1 Matroska index available", demuxer.index_navigation_supported(), true);

  const auto next = demuxer.adjacent_keyframe(+1, 0.0F);
  expect_bool("FFV1 next keyframe available", next.status == playback_keyframe::AdjacentKeyframeStatus::Ok, true);
  if (next.status == playback_keyframe::AdjacentKeyframeStatus::Ok) {
    expect_bool("FFV1 next keyframe follows first", next.demuxer_position_seconds > 0.0F, true);
  }

  const bool preroll = demuxer.seek_keyframe_preroll(0.0F);
  expect_bool("FFV1 first-keyframe preroll succeeds", preroll, true);
  if (preroll) {
    AVPacket packet{};
    const bool read = demuxer(packet);
    expect_bool("FFV1 first-keyframe preroll lands at start", read && packet.stream_index == demuxer.video_stream_index() && packet.pts == 0, true);
    av_packet_unref(&packet);
  }
}

static void test_pipe_nonseekable_rejects_without_probe(const std::string& source_path) {
  const std::string fifo = std::string("/tmp/vc_kf_pipe_") + std::to_string(getpid());
  if (mkfifo(fifo.c_str(), 0600) != 0) {
    std::fprintf(stderr, "FAIL could not create fifo %s\n", fifo.c_str());
    failures++;
    return;
  }

  const pid_t feeder = start_pipe_feeder(source_path, fifo);
  if (feeder <= 0) {
    unlink(fifo.c_str());
    std::fprintf(stderr, "FAIL could not start pipe feeder\n");
    failures++;
    return;
  }

  try {
    Demuxer demuxer(Side::Right(), "", fifo, nullptr, nullptr);
    expect_bool("pipe keyframe_navigation_seekable", demuxer.keyframe_navigation_seekable(), false);

    const auto start = std::chrono::steady_clock::now();
    expect_bool("pipe index_navigation_supported", demuxer.index_navigation_supported(), false);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (elapsed_ms > 500) {
      std::fprintf(stderr, "FAIL pipe index gate took %lld ms (expected immediate)\n", static_cast<long long>(elapsed_ms));
      failures++;
    } else {
      std::printf("PASS pipe index gate immediate (%lld ms)\n", static_cast<long long>(elapsed_ms));
    }
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "FAIL pipe demuxer open: %s\n", ex.what());
    failures++;
  }

  stop_child(feeder);
  unlink(fifo.c_str());
}

static void test_resolve_step_mixed_file_and_pipe(const std::string& file_path, const std::string& pipe_source) {
  const std::string fifo = std::string("/tmp/vc_kf_mix_") + std::to_string(getpid());
  if (mkfifo(fifo.c_str(), 0600) != 0) {
    std::fprintf(stderr, "FAIL could not create mixed fifo\n");
    failures++;
    return;
  }

  const pid_t feeder = start_pipe_feeder(pipe_source, fifo);
  if (feeder <= 0) {
    unlink(fifo.c_str());
    failures++;
    return;
  }

  try {
    Demuxer left(Side::Left(), "", file_path, nullptr, nullptr);
    Demuxer right(Side::Right(), "", fifo, nullptr, nullptr);
    expect_bool("mixed left seekable", left.keyframe_navigation_seekable(), true);
    expect_bool("mixed right not seekable", right.keyframe_navigation_seekable(), false);

    playback_keyframe::KeyframeNavigation navigation;
    navigation.begin_frame(+1, true);
    const playback_seek::SeekRequest request = sample_seek_request();
    const auto start = std::chrono::steady_clock::now();
    const playback_keyframe::KeyframeNavigationResolve resolved = navigation.resolve_step(+1, left, right, request, 0, 0.0F, true, 0.0F, true);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    expect_status("mixed resolve_step unavailable", resolved.status, playback_keyframe::KeyframeNavigationStatus::Unavailable);
    expect_bool("mixed resolve_step clears pending", navigation.has_pending_steps(), false);
    if (elapsed_ms > 500) {
      std::fprintf(stderr, "FAIL mixed resolve_step took %lld ms\n", static_cast<long long>(elapsed_ms));
      failures++;
    } else {
      std::printf("PASS mixed resolve_step immediate (%lld ms)\n", static_cast<long long>(elapsed_ms));
    }
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "FAIL mixed demuxer test: %s\n", ex.what());
    failures++;
  }

  stop_child(feeder);
  unlink(fifo.c_str());
}

int main(int argc, char** argv) {
  const std::string fixture = fixture_path(argc, argv);
  if (fixture.empty()) {
    std::printf("SKIP demuxer keyframe seekable tests (pass fixture path or set KEYFRAME_TEST_FIXTURE)\n");
    return EXIT_SUCCESS;
  }

  try {
    test_file_seekable_and_index_gate(fixture);
    for (int i = 2; i < argc; ++i) {
      test_ffv1_matroska_navigation(argv[i]);
    }
    test_pipe_nonseekable_rejects_without_probe(fixture);
    test_resolve_step_mixed_file_and_pipe(fixture, fixture);
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "FAIL unexpected: %s\n", ex.what());
    return EXIT_FAILURE;
  }

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }

  std::printf("All demuxer keyframe seekable tests passed\n");
  return EXIT_SUCCESS;
}

#endif