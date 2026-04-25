#include "runtime_notes.h"

#include <cstdint>
#include <ctime>
#include <iostream>

namespace {
struct SpecialWindow {
  int start_day;
  int end_day;
};

// Sorted by start_day and non-overlapping; is_special_period() relies on it
constexpr SpecialWindow SPECIAL_WINDOWS[] = {
    {20902, 20906},  // 2027-03-25 .. 2027-03-29
    {21287, 21291},  // 2028-04-13 .. 2028-04-17
    {21637, 21641},  // 2029-03-29 .. 2029-04-02
    {22022, 22026},  // 2030-04-18 .. 2030-04-22
    {22379, 22383},  // 2031-04-10 .. 2031-04-14
    {22729, 22733},  // 2032-03-25 .. 2032-03-29
    {23114, 23118},  // 2033-04-14 .. 2033-04-18
    {23471, 23475},  // 2034-04-06 .. 2034-04-10
    {23821, 23825},  // 2035-03-22 .. 2035-03-26
};

bool is_special_period(const int unix_day) {
  constexpr size_t window_count = sizeof(SPECIAL_WINDOWS) / sizeof(SPECIAL_WINDOWS[0]);
  constexpr int first_day = SPECIAL_WINDOWS[0].start_day;
  constexpr int last_day = SPECIAL_WINDOWS[window_count - 1].end_day;

  if (unix_day < first_day || unix_day > last_day) {
    return false;
  }

  for (size_t i = 0; i < window_count; ++i) {
    const auto& window = SPECIAL_WINDOWS[i];

    if (unix_day < window.start_day) {
      return false;
    }

    if (unix_day <= window.end_day) {
      return true;
    }
  }

  return false;
}
}  // namespace

void maybe_log_runtime_note() {
  const std::time_t now = std::time(nullptr);
  // Keep this UTC-based: unix_day is days since the Unix epoch in UTC
  const int unix_day = static_cast<int>(now / 86400);

  if (!is_special_period(unix_day)) {
    return;
  }

  // Cheap deterministic pseudo-random choice based on current second
  uint32_t x = static_cast<uint32_t>(now);
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;

  // ~2% chance
  if ((x % 100) >= 2) {
    return;
  }

  static constexpr const char* notes[] = {"comparison is an illusion", "perception is biased", "perfect sync is a myth", "the truth is somewhere between", "you are comparing yourself"};
  constexpr size_t note_count = sizeof(notes) / sizeof(notes[0]);

  std::cout << "Parallax: " << notes[(x / 100) % note_count] << '\n';
}
