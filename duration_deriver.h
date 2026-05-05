#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>

class DurationDeriver {
 public:
  enum class Source { PtsDelta, Prediction, Metadata, Fallback };

  struct Input {
    int64_t frame_field_duration{0};
    int64_t metadata_duration{0};
    int64_t current_pts{0};
    int64_t previous_pts{0};
    bool has_previous_pts{false};
  };

  struct Result {
    int64_t resolved_duration{0};
    Source source{Source::Fallback};
    int64_t pts_delta{0};
    int64_t predicted_duration{0};
    int64_t metadata_duration{0};
    bool source_changed{false};
    bool has_previous_source{false};
    Source previous_source{Source::Fallback};
  };

  explicit DurationDeriver(size_t history_window = 12);

  Result derive(const Input& input);

  void reset();

  static const char* source_name(const Source source);

 private:
  int64_t predict_duration() const;
  bool is_plausible_duration_delta(const int64_t duration, const int64_t anchor) const;
  void remember_duration(const int64_t duration);

  size_t history_window_;
  std::deque<int64_t> duration_history_;
  bool has_last_source_;
  Source last_source_;
};
