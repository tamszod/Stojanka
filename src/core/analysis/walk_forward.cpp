#include "core/analysis/walk_forward.h"

#include <algorithm>

#include "core/analysis/analyzer.h"
#include "core/analysis/calibrator.h"
#include "core/analysis/indicators.h"

namespace strojanka::analysis {

WalkForwardResult walk_forward(const std::vector<Bar>& bars, size_t train_window, size_t step) {
    WalkForwardResult r;
    constexpr size_t kMinTrain = 250;
    if (bars.size() < kMinTrain + step) return r;

    AnalysisParams params;
    std::string last_peak_leg, last_support_leg;
    for (size_t start = kMinTrain; start < bars.size(); start += step) {
        const size_t train_from = start > train_window ? start - train_window : 0;
        const std::vector<Bar> train(bars.begin() + static_cast<std::ptrdiff_t>(train_from),
                                     bars.begin() + static_cast<std::ptrdiff_t>(start));
        params = calibrate(train, params).params;
        ++r.segments;

        // The live monitor analyses the most recent train_window bars; mirror that window.
        const size_t end = std::min(bars.size(), start + step);
        const std::vector<Bar> window(bars.begin() + static_cast<std::ptrdiff_t>(train_from),
                                      bars.begin() + static_cast<std::ptrdiff_t>(end));
        const auto closes = closes_of(window);
        const auto states = analyze_series(closes, params);
        for (size_t i = start - train_from; i < window.size(); ++i) {
            const size_t global = train_from + i;
            for (const SignalSide any : {SignalSide::Peak, SignalSide::Support}) {
                const SignalOutcome o = evaluate_outcome(bars, global, bars[global].close, any, params.move_pct, params.horizon_bars);
                if (o == SignalOutcome::Pending) continue;
                const bool ok = o == SignalOutcome::Correct;
                if (any == SignalSide::Peak) {
                    ++r.baseline_peak_days;
                    r.baseline_peak_correct += ok;
                } else {
                    ++r.baseline_support_days;
                    r.baseline_support_correct += ok;
                }
            }
            const BarState& s = states[i];
            if (!s.valid) continue;
            const std::string& leg = window[s.leg_start].date;
            SignalSide side;
            if (s.direction > 0 && s.peak_score >= params.peak_cutoff() && leg != last_peak_leg) {
                side = SignalSide::Peak;
                last_peak_leg = leg;
            } else if (s.direction < 0 && s.support_score >= params.support_cutoff() && leg != last_support_leg) {
                side = SignalSide::Support;
                last_support_leg = leg;
            } else {
                continue;
            }
            const SignalOutcome o = evaluate_outcome(bars, global, bars[global].close, side, params.move_pct, params.horizon_bars);
            if (o == SignalOutcome::Pending) continue;
            const bool ok = o == SignalOutcome::Correct;
            if (side == SignalSide::Peak) {
                ++r.peak_signals;
                r.peak_correct += ok;
            } else {
                ++r.support_signals;
                r.support_correct += ok;
            }
        }
    }
    return r;
}

}  // namespace strojanka::analysis
