#include "duration_deriver.h"
#include <algorithm>
#include <limits>

constexpr size_t DurationDeriver::HISTORY_WINDOW;

DurationDeriver::DurationDeriver() : history_size_(0), next_index_(0), duration_history_{}, has_last_source_(false), last_source_(Source::Fallback) {}

DurationDeriver::Result DurationDeriver::derive(const Input& input) {
  Result result;
  result.predicted_duration = predict_duration();
  result.metadata_duration = input.metadata_duration;
  bool metadata_plausible = false;

  if (input.has_previous_pts) {
    if (input.current_pts > input.previous_pts) {
      const bool subtraction_overflow = input.previous_pts < 0 && input.current_pts > (std::numeric_limits<int64_t>::max() + input.previous_pts);
      if (!subtraction_overflow) {
        result.pts_delta = input.current_pts - input.previous_pts;
      }
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
    const int64_t metadata_anchor = result.predicted_duration > 0 ? result.predicted_duration : input.frame_field_duration;
    if (metadata_anchor > 0) {
      metadata_plausible = is_plausible_duration_delta(result.metadata_duration, metadata_anchor);
    }

    if (metadata_plausible) {
      result.resolved_duration = result.metadata_duration;
      result.source = Source::Metadata;
    }
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

  const bool remember_resolved_duration =
      result.source == Source::PtsDelta ||
      result.source == Source::Prediction ||
      (result.source == Source::Metadata && metadata_plausible);
  if (remember_resolved_duration) {
    remember_duration(result.resolved_duration);
  }

  result.has_previous_source = has_last_source_;
  result.previous_source = last_source_;
  result.source_changed = !has_last_source_ || result.source != last_source_;

  has_last_source_ = true;
  last_source_ = result.source;

  return result;
}

void DurationDeriver::reset() {
  history_size_ = 0;
  next_index_ = 0;
  has_last_source_ = false;
  last_source_ = Source::Fallback;
}

const char* DurationDeriver::source_name(const Source source) {
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
  if (history_size_ == 0) {
    return 0;
  }

  std::array<int64_t, HISTORY_WINDOW> sorted_durations{};
  for (size_t i = 0; i < history_size_; ++i) {
    sorted_durations[i] = duration_history_[i];
  }

  // Small bounded window: insertion sort is simple and allocation-free.
  for (size_t i = 1; i < history_size_; ++i) {
    const int64_t key = sorted_durations[i];
    size_t j = i;
    while (j > 0 && sorted_durations[j - 1] > key) {
      sorted_durations[j] = sorted_durations[j - 1];
      --j;
    }
    sorted_durations[j] = key;
  }

  const size_t mid = history_size_ / 2;
  if ((history_size_ % 2) == 0) {
    const int64_t left = sorted_durations[mid - 1];
    const int64_t right = sorted_durations[mid];
    return left / 2 + right / 2 + ((left % 2 + right % 2) / 2);
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
  const int64_t max_int64 = std::numeric_limits<int64_t>::max();
  const int64_t scaled_upper = anchor > (max_int64 / 4) ? max_int64 : anchor * 4;
  const int64_t upper_bound = std::max<int64_t>(lower_bound, scaled_upper);

  return duration >= lower_bound && duration <= upper_bound;
}

void DurationDeriver::remember_duration(const int64_t duration) {
  if (duration <= 0) {
    return;
  }

  duration_history_[next_index_] = duration;
  if (history_size_ < HISTORY_WINDOW) {
    ++history_size_;
  }
  next_index_ = (next_index_ + 1) % HISTORY_WINDOW;
}
