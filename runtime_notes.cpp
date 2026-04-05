#include "runtime_notes.h"

#include <cstdlib>
#include <ctime>
#include <iostream>

namespace {
struct SpecialWindow {
  int year;
  int start_month;
  int start_day;
  int end_month;
  int end_day;
};

constexpr SpecialWindow SPECIAL_WINDOWS[] = {{2026, 4, 3, 4, 6},   {2027, 3, 26, 3, 29}, {2028, 4, 14, 4, 17}, {2029, 3, 30, 4, 2}, {2030, 4, 19, 4, 22},
                                             {2031, 4, 11, 4, 14}, {2032, 3, 26, 3, 29}, {2033, 4, 15, 4, 18}, {2034, 4, 7, 4, 10}, {2035, 3, 23, 3, 26}};

bool is_special_period(int year, int month, int day) {
  for (const auto& window : SPECIAL_WINDOWS) {
    if (window.year != year) {
      continue;
    }
    const bool after_start = month > window.start_month || (month == window.start_month && day >= window.start_day);
    const bool before_end = month < window.end_month || (month == window.end_month && day <= window.end_day);
    return after_start && before_end;
  }
  return false;
}
}  // namespace

void maybe_log_runtime_note() {
  std::time_t now = std::time(nullptr);
  std::tm* local_time = std::localtime(&now);
  if (!local_time) {
    return;
  }

  const int year = local_time->tm_year + 1900;
  const int month = local_time->tm_mon + 1;
  const int day = local_time->tm_mday;
  if (!is_special_period(year, month, day)) {
    return;
  }

  std::srand(static_cast<unsigned int>(now));
  if ((std::rand() % 100) >= 2) {
    return;
  }

  static const char* const notes[] = {"comparison is an illusion...", "perception is biased", "perfect sync is a myth", "the truth is somewhere between...", "you are comparing yourself..."};
  const size_t note_count = sizeof(notes) / sizeof(notes[0]);

  std::cout << "Parallax: " << notes[std::rand() % note_count] << std::endl;
}
