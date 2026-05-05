#include "duration_deriver.h"
#include <algorithm>
#include <vector>

DurationDeriver::DurationDeriver(size_t history_window) : history_window_(history_window > 0 ? history_window : 1), has_last_source_(false), last_source_(Source::Fallback) {}

DurationDeriver::Result DurationDeriver::derive(const Input& input) {
  Result result;
  result.predicted_duration = predict_duration();
  result.metadata_duration = input.metadata_duration;

  if (input.has_previous_pts) {
    const int64_t delta = input.current_pts - input.previous_pts;
    if (delta > 0) {
      result.pts_delta = delta;
    }
  }

  // Layer 1: prefer current PTS delta when it is plausible (best for VFR).
  if (result.pts_delta > 0) {
    const int64_t plausibility_anchor = result.predicted_duration > 0 ? result.predicted_duration : result.metadata_duration;
    if (is_plausible_duration_delta(result.pts_delta, plausibility_anchor)) {
      result.resolved_duration = result.pts_delta;
      result.source = Source::PtsDelta;
    }
  }

  // Layer 2: predict from recent validated history.
  if (result.resolved_duration <= 0 && result.predicted_duration > 0) {
    result.resolved_duration = result.predicted_duration;
    result.source = Source::Prediction;
  }

  // Layer 3: use metadata baseline if no dynamic value exists yet.
  if (result.resolved_duration <= 0 && result.metadata_duration > 0) {
    result.resolved_duration = result.metadata_duration;
    result.source = Source::Metadata;
  }

  // Layer 4: consolidated last-resort fallback for broken/missing timing metadata.
  if (result.resolved_duration <= 0 && input.frame_field_duration > 0) {
    result.resolved_duration = input.frame_field_duration;
    result.source = Source::Fallback;
  }
  if (result.resolved_duration <= 0) {
    result.resolved_duration = 1;
    result.source = Source::Fallback;
  }

  remember_duration(result.resolved_duration);

  result.has_previous_source = has_last_source_;
  result.previous_source = last_source_;
  result.source_changed = !has_last_source_ || result.source != last_source_;

  has_last_source_ = true;
  last_source_ = result.source;

  return result;
}

void DurationDeriver::reset() {
  duration_history_.clear();
  has_last_source_ = false;
  last_source_ = Source::Fallback;
}

const char* DurationDeriver::source_name(Source source) {
  switch (source) {
    case Source::PtsDelta:
      return "pts_delta";
    case Source::Prediction:
      return "prediction";
    case Source::Metadata:
      return "metadata";
    case Source::Fallback:
      return "fallback";
  }

  return "unknown";
}

int64_t DurationDeriver::predict_duration() const {
  if (duration_history_.empty()) {
    return 0;
  }

  std::vector<int64_t> sorted_durations(duration_history_.begin(), duration_history_.end());
  std::sort(sorted_durations.begin(), sorted_durations.end());

  const size_t mid = sorted_durations.size() / 2;
  if ((sorted_durations.size() % 2) == 0) {
    return (sorted_durations[mid - 1] + sorted_durations[mid]) / 2;
  }

  return sorted_durations[mid];
}

bool DurationDeriver::is_plausible_duration_delta(const int64_t duration, const int64_t anchor) const {
  if (duration <= 0) {
    return false;
  }
  if (anchor <= 0) {
    return true;
  }

  const int64_t lower_bound = std::max<int64_t>(1, anchor / 4);
  const int64_t upper_bound = std::max<int64_t>(lower_bound, anchor * 4);

  return duration >= lower_bound && duration <= upper_bound;
}

void DurationDeriver::remember_duration(const int64_t duration) {
  if (duration <= 0) {
    return;
  }

  duration_history_.push_back(duration);
  while (duration_history_.size() > history_window_) {
    duration_history_.pop_front();
  }
}
