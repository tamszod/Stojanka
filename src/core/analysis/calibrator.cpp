#include "core/analysis/calibrator.h"

#include <algorithm>
#include <cmath>

#include "core/analysis/indicators.h"
#include "core/analysis/zigzag.h"

namespace strojanka::analysis {

namespace {

constexpr size_t kMinBars = 120;
constexpr size_t kEvalStart = 60;
constexpr double kHalfLifeBars = 250;  // a call one year ago counts half as much as one today

const double kSwingFactors[] = {0.6, 0.8, 1.0, 1.3, 1.7};
const double kRsiHigh[] = {62, 68, 74};
const double kRsiLow[] = {38, 32, 26};
const double kThresholds[] = {0.50, 0.55, 0.60, 0.65, 0.70, 0.75, 0.80, 0.85};

struct Candidate {
    double rsi = 0;
    double threshold = 0;
    SideStats stats;
};

SideStats simulate(const std::vector<BarState>& states, const std::vector<SignalOutcome>& outcomes, bool peak,
                   double threshold, size_t reference_legs) {
    SideStats s;
    const size_t n = states.size();
    double weight_total = 0, weight_correct = 0;
    size_t last_leg = static_cast<size_t>(-1);
    for (size_t i = kEvalStart; i < n; ++i) {
        const BarState& b = states[i];
        if (!b.valid || b.direction != (peak ? 1 : -1)) continue;
        const double score = peak ? b.peak_score : b.support_score;
        if (score < threshold || b.leg_start == last_leg) continue;
        last_leg = b.leg_start;  // one call per swing, exactly like the live monitor
        const SignalOutcome o = outcomes[i];
        if (o == SignalOutcome::Pending) continue;
        const double w = std::pow(0.5, static_cast<double>(n - 1 - i) / kHalfLifeBars);
        weight_total += w;
        ++s.signals;
        if (o == SignalOutcome::Correct) {
            weight_correct += w;
            ++s.correct;
        }
    }
    s.precision = (weight_correct + 1) / (weight_total + 2);
    s.coverage = std::min(1.0, s.signals / std::max(1.0, 0.6 * static_cast<double>(reference_legs)));
    s.objective = s.precision * (0.5 + 0.5 * s.coverage);
    return s;
}

}  // namespace

AnalysisParams default_params(const std::vector<Bar>& bars) {
    const double dv = daily_volatility(closes_of(bars), 252);
    AnalysisParams p;
    p.swing_pct = std::clamp(4.0 * dv, 0.03, 0.30);
    p.move_pct = std::clamp(2.5 * dv, 0.015, 0.15);
    p.horizon_bars = 20;
    return p;
}

CalibrationResult calibrate(const std::vector<Bar>& bars, const AnalysisParams& previous) {
    CalibrationResult result;
    AnalysisParams base = default_params(bars);
    base.peak_bias = previous.peak_bias;
    base.support_bias = previous.support_bias;
    result.params = base;
    if (bars.size() < kMinBars) return result;
    result.enough_data = true;

    const auto closes = closes_of(bars);
    const auto indicators = compute_indicators(closes);
    const size_t n = bars.size();

    std::vector<SignalOutcome> peak_outcomes(n), support_outcomes(n);
    for (size_t i = 0; i < n; ++i) {
        peak_outcomes[i] = evaluate_outcome(bars, i, closes[i], SignalSide::Peak, base.move_pct, base.horizon_bars);
        support_outcomes[i] = evaluate_outcome(bars, i, closes[i], SignalSide::Support, base.move_pct, base.horizon_bars);
    }

    size_t ref_highs = 0, ref_lows = 0;
    for (const auto& pv : zigzag_pivots(closes, base.swing_pct)) {
        if (pv.index < kEvalStart) continue;
        (pv.is_high ? ref_highs : ref_lows)++;
    }

    double best_total = -1;
    for (double factor : kSwingFactors) {
        Candidate best_peak, best_support;
        best_peak.stats.objective = best_support.stats.objective = -1;
        for (size_t r = 0; r < std::size(kRsiHigh); ++r) {
            AnalysisParams p = base;
            p.swing_pct = std::clamp(base.swing_pct * factor, 0.02, 0.45);
            p.rsi_high = kRsiHigh[r];
            p.rsi_low = kRsiLow[r];
            const auto states = analyze_series(closes, indicators, p);
            for (double thr : kThresholds) {
                const SideStats ps = simulate(states, peak_outcomes, true, thr, ref_highs);
                const SideStats ss = simulate(states, support_outcomes, false, thr, ref_lows);
                // Thresholds ascend, so ">=" settles ties on the stricter threshold.
                if (ps.objective >= best_peak.stats.objective) best_peak = {p.rsi_high, thr, ps};
                if (ss.objective >= best_support.stats.objective) best_support = {p.rsi_low, thr, ss};
            }
        }
        const double total = best_peak.stats.objective + best_support.stats.objective;
        if (total > best_total) {
            best_total = total;
            result.params = base;
            result.params.swing_pct = std::clamp(base.swing_pct * factor, 0.02, 0.45);
            result.params.rsi_high = best_peak.rsi;
            result.params.peak_threshold = best_peak.threshold;
            result.params.rsi_low = best_support.rsi;
            result.params.support_threshold = best_support.threshold;
            result.peak = best_peak.stats;
            result.support = best_support.stats;
        }
    }
    result.score = best_total / 2;
    return result;
}

double adjust_bias(double bias, const std::vector<SignalRecord>& recent) {
    const size_t n = std::min<size_t>(recent.size(), 10);
    if (n < 3) return bias;
    size_t correct = 0;
    for (size_t i = 0; i < n; ++i) {
        if (recent[i].outcome == SignalOutcome::Correct) ++correct;
    }
    const double precision = static_cast<double>(correct) / static_cast<double>(n);
    if (precision < 0.5) return std::min(0.15, bias + 0.03);
    if (precision >= 0.75) return std::max(-0.05, bias - 0.01);
    return bias;
}

}  // namespace strojanka::analysis
