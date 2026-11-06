#include "duration_deriver.h"

#include <cstdint>
#include <cstdio>

namespace {

int g_failures = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  }
}

void expect_eq(int64_t a, int64_t b, const char* msg) {
  if (a != b) {
    std::fprintf(stderr, "FAIL: %s (got %lld expected %lld)\n", msg, (long long)a, (long long)b);
    ++g_failures;
  }
}

void expect_src(DurationDeriver::Source s, DurationDeriver::Source e, const char* msg) {
  if (s != e) {
    std::fprintf(stderr, "FAIL: %s (source mismatch)\n", msg);
    ++g_failures;
  }
}

}  // namespace

int main() {
  // --- CFR 25 fps at 90 kHz ticks: one frame = 3600 ticks ---
  {
    DurationDeriver d;
    const int64_t tick = 3600;
    auto r = d.derive({tick, tick, tick, 0, false});
    expect_src(r.source, DurationDeriver::Source::Metadata, "cfr1 metadata when no pts");
    expect_eq(r.resolved_duration, tick, "cfr1");

    r = d.derive({tick, tick, tick + tick, tick, true});
    expect_src(r.source, DurationDeriver::Source::PtsDelta, "cfr2 pts delta");
    expect_eq(r.resolved_duration, tick, "cfr2");
  }

  // --- VFR: irregular but plausible PTS deltas (anchor from history) ---
  {
    DurationDeriver d;
    const int64_t meta = 3000;
    d.derive({meta, meta, 1000, 0, false});
    d.derive({meta, meta, 1000 + 5000, 1000, true});
    auto r = d.derive({meta, meta, 1000 + 5000 + 1500, 1000 + 5000, true});
    expect_src(r.source, DurationDeriver::Source::PtsDelta, "vfr pts");
    expect_eq(r.resolved_duration, 1500, "vfr delta");
  }

  // --- Missing PTS: stay on metadata when plausible ---
  {
    DurationDeriver d;
    const int64_t m = 2400;
    auto r = d.derive({m, m, 0, 0, false});
    expect_eq(r.resolved_duration, m, "no pts metadata");
    expect_src(r.source, DurationDeriver::Source::Metadata, "no pts src");
  }

  // --- Non-increasing PTS (rewind): no pts delta; prediction or fallback ---
  {
    DurationDeriver d;
    const int64_t t = 3000;
    d.derive({t, t, 100000, 0, false});
    d.derive({t, t, 100000 + t, 100000, true});
    auto r = d.derive({t, t, 50000, 100000 + t, true});
    expect_eq(r.pts_delta, 0, "rewind no delta");
    expect(r.resolved_duration > 0, "rewind resolved");
  }

  // With no history, a large PTS delta consistent with the supplied
  // metadata may be a legitimate sparse VFR interval, even when the nominal
  // frame-rate estimate suggests a much shorter frame duration.
  {
    DurationDeriver d;
    const int64_t nominal = 3600;
    const int64_t gap = nominal * 400;
    auto r = d.derive({nominal, gap, 1000 + gap, 1000, true});
    expect_src(r.source, DurationDeriver::Source::PtsDelta, "cold-start sparse VFR source");
    expect_eq(r.resolved_duration, gap, "cold-start sparse VFR duration");
  }

  // Once a reliable cadence exists, reject large, inconsistent PTS deltas
  // rather than letting them displace the recent duration estimate.
  {
    DurationDeriver d;
    const int64_t nominal = 3600;
    d.derive({nominal, nominal, 1000, 0, false});
    d.derive({nominal, nominal, 1000 + nominal, 1000, true});
    const int64_t bad_delta = nominal * 400;
    auto r = d.derive({nominal, bad_delta, 1000 + nominal + bad_delta, 1000 + nominal, true});
    expect_src(r.source, DurationDeriver::Source::Prediction, "reject outlier against history");
    expect_eq(r.resolved_duration, nominal, "predict normal duration after outlier");
  }

  // --- Reset clears history ---
  {
    DurationDeriver d;
    const int64_t t = 2000;
    d.derive({t, t, t, 0, false});
    d.derive({t, t, 2 * t, t, true});
    d.reset();
    auto r = d.derive({t, t, 100 * t, 0, false});
    expect_eq(r.predicted_duration, 0, "reset clears predict");
  }

  // --- Layer 3 metadata uses frame_field anchor when prediction is cold (not self-validate) ---
  {
    DurationDeriver d;
    const int64_t nominal = 3600;
    const int64_t meta_ok = nominal * 2;
    auto r = d.derive({nominal, meta_ok, 0, 0, false});
    expect_src(r.source, DurationDeriver::Source::Metadata, "meta vs nominal anchor");
    expect_eq(r.resolved_duration, meta_ok, "meta 2x nominal");
  }

  // --- Identical PTS with huge metadata: no PTS delta; reject bad metadata when anchored to nominal ---
  {
    DurationDeriver d;
    const int64_t nominal = 3600;
    const int64_t same_ts = 5000000;
    const int64_t bad_meta = nominal * 400;
    auto r = d.derive({nominal, bad_meta, same_ts, same_ts, true});
    expect_eq(r.pts_delta, 0, "dup pts delta");
    expect_src(r.source, DurationDeriver::Source::Fallback, "dup pts huge meta");
    expect_eq(r.resolved_duration, nominal, "dup nominal");
  }

  // --- 2^33 wrap style step: strict > comparison yields no delta; do not invent from wrap here ---
  {
    DurationDeriver d;
    const int64_t nominal = 3600;
    constexpr int64_t kWrap33 = 8589934592LL;
    const int64_t prev = kWrap33 - nominal;
    const int64_t curr = 0LL;
    auto r = d.derive({nominal, nominal, curr, prev, true});
    expect_eq(r.pts_delta, 0, "wrap no strict delta");
    expect(r.resolved_duration > 0, "wrap still resolves");
  }

  // A 1-second VFR interval is valid when the PTS and metadata agree.
  {
    DurationDeriver d;
    const int64_t nominal = 3600;
    const int64_t one_second = 90000;
    auto r = d.derive({nominal, one_second, one_second + 1000, 1000, true});
    expect_src(r.source, DurationDeriver::Source::PtsDelta, "cold sparse VFR PTS delta");
    expect_eq(r.resolved_duration, one_second, "cold sparse VFR 1 second");
  }

  // A 400-second gap can also be valid for sparse or image-based VFR input.
  {
    DurationDeriver d;
    const int64_t nominal = 3600;
    const int64_t long_gap = 400LL * 90000;
    auto r = d.derive({nominal, long_gap, 1000 + long_gap, 1000, true});
    expect_src(r.source, DurationDeriver::Source::PtsDelta, "sparse VFR 400s source");
    expect_eq(r.resolved_duration, long_gap, "sparse VFR 400s duration");
  }

  // With no usable frame-rate estimate, a huge metadata duration is not
  // sufficient to establish a valid duration on its own.
  {
    DurationDeriver d;
    auto r = d.derive({0, 1000000000000LL, 10000, 0, false});
    expect_src(r.source, DurationDeriver::Source::Fallback, "zero nominal huge metadata fallback");
    expect_eq(r.resolved_duration, 1, "zero nominal returns bounded fallback");
  }

  // Extremely large positive frame duration must never cause signed overflow.
  {
    DurationDeriver d;
    const int64_t giant = INT64_MAX / 256 + 1;
    auto r = d.derive({giant, 1, 1, 0, true});
    expect(r.resolved_duration > 0, "huge nominal safe without UB");
  }

  std::fprintf(stderr, "duration_deriver_test: %d failures\n", g_failures);
  return g_failures > 0 ? 1 : 0;
}
