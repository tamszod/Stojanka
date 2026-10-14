#include "core/analysis/analyzer.h"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "core/analysis/indicators.h"
#include "core/analysis/zigzag.h"

namespace strojanka::analysis {

namespace {
constexpr size_t kWarmupBars = 30;
constexpr size_t kAmplitudeHistory = 8;  // pivots used to estimate a "typical" swing
constexpr size_t kLevelPivots = 3;       // pivots averaged into support/resistance

double clamp_cutoff(double v) { return std::min(0.97, std::max(0.35, v)); }
}  // namespace

double AnalysisParams::peak_cutoff() const { return clamp_cutoff(peak_threshold + peak_bias); }
double AnalysisParams::support_cutoff() const { return clamp_cutoff(support_threshold + support_bias); }

std::string AnalysisParams::to_json() const {
    nlohmann::json j = {
        {"swing_pct", swing_pct},           {"rsi_high", rsi_high},
        {"rsi_low", rsi_low},               {"peak_threshold", peak_threshold},
        {"support_threshold", support_threshold}, {"peak_bias", peak_bias},
        {"support_bias", support_bias},     {"move_pct", move_pct},
        {"horizon_bars", horizon_bars},
    };
    return j.dump();
}

std::optional<AnalysisParams> AnalysisParams::from_json(const std::string& text) {
    try {
        const auto j = nlohmann::json::parse(text);
        AnalysisParams p;
        p.swing_pct = j.value("swing_pct", p.swing_pct);
        p.rsi_high = j.value("rsi_high", p.rsi_high);
        p.rsi_low = j.value("rsi_low", p.rsi_low);
        p.peak_threshold = j.value("peak_threshold", p.peak_threshold);
        p.support_threshold = j.value("support_threshold", p.support_threshold);
        p.peak_bias = j.value("peak_bias", p.peak_bias);
        p.support_bias = j.value("support_bias", p.support_bias);
        p.move_pct = j.value("move_pct", p.move_pct);
        p.horizon_bars = j.value("horizon_bars", p.horizon_bars);
        return p;
    } catch (...) {
        return std::nullopt;
    }
}

Indicators compute_indicators(const std::vector<double>& closes) {
    return Indicators{rsi(closes, 14), percent_b(closes, 20, 2.0)};
}

std::vector<BarState> analyze_series(const std::vector<double>& closes, const Indicators& ind,
                                     const AnalysisParams& p) {
    std::vector<BarState> out(closes.size());
    ZigZag zz(p.swing_pct);
    const double proximity = std::max(0.01, 0.5 * p.swing_pct);
    const double turn_scale = 0.35 * p.swing_pct;

    for (size_t i = 0; i < closes.size(); ++i) {
        zz.update(i, closes[i]);
        BarState& s = out[i];
        s.direction = zz.direction();
        s.rsi = std::isnan(ind.rsi[i]) ? 50.0 : ind.rsi[i];
        s.percent_b = std::isnan(ind.percent_b[i]) ? 0.5 : ind.percent_b[i];
        const auto& pv = zz.pivots();
        if (i < kWarmupBars || pv.empty() || s.direction == 0) continue;
        s.valid = true;

        std::vector<double> ups, downs, highs, lows;
        const size_t first = pv.size() > kAmplitudeHistory ? pv.size() - kAmplitudeHistory : 0;
        for (size_t k = first; k < pv.size(); ++k) {
            if (k > first) {
                const Pivot& a = pv[k - 1];
                const Pivot& b = pv[k];
                if (!a.is_high && b.is_high) ups.push_back(b.price / a.price - 1);
                if (a.is_high && !b.is_high) downs.push_back(1 - b.price / a.price);
            }
        }
        for (auto it = pv.rbegin(); it != pv.rend(); ++it) {
            auto& bucket = it->is_high ? highs : lows;
            if (bucket.size() < kLevelPivots) bucket.push_back(it->price);
            if (highs.size() >= kLevelPivots && lows.size() >= kLevelPivots) break;
        }
        const size_t window_start = i >= 60 ? i - 60 : 0;
        const auto window_begin = closes.begin() + static_cast<std::ptrdiff_t>(window_start);
        const auto window_end = closes.begin() + static_cast<std::ptrdiff_t>(i + 1);
        auto mean = [](const std::vector<double>& v) {
            double t = 0;
            for (double x : v) t += x;
            return t / static_cast<double>(v.size());
        };
        s.resistance_level = highs.empty() ? *std::max_element(window_begin, window_end) : mean(highs);
        s.support_level = lows.empty() ? *std::min_element(window_begin, window_end) : mean(lows);
        const double typical_up = std::max(p.swing_pct, ups.empty() ? 2 * p.swing_pct : median(ups));
        const double typical_down = std::max(p.swing_pct * 0.8, downs.empty() ? 1.6 * p.swing_pct : median(downs));

        const double c = closes[i];
        const Pivot& origin = pv.back();
        s.leg_start = origin.index;
        s.leg_origin_price = origin.price;

        if (s.direction > 0) {
            const double gain = c / origin.price - 1;
            const double s_amp = smoothstep(0.45, 1.0, gain / typical_up);
            const double s_level = 1 - clamp01(std::fabs(c / s.resistance_level - 1) / proximity);
            const double s_rsi = smoothstep(50, p.rsi_high, s.rsi);
            const double s_band = smoothstep(0.6, 1.0, s.percent_b);
            const double s_turn = smoothstep(0, turn_scale, 1 - c / zz.extreme());
            s.peak_score = 0.30 * s_amp + 0.20 * s_level + 0.25 * s_rsi + 0.10 * s_band + 0.15 * s_turn;
        } else {
            const double drop = 1 - c / origin.price;
            const double s_amp = smoothstep(0.45, 1.0, drop / typical_down);
            const double s_level = 1 - clamp01(std::fabs(c / s.support_level - 1) / proximity);
            const double s_rsi = smoothstep(50, p.rsi_low, s.rsi);
            const double s_band = smoothstep(0.4, 0.0, s.percent_b);
            const double s_turn = smoothstep(0, turn_scale, c / zz.extreme() - 1);
            s.support_score = 0.30 * s_amp + 0.20 * s_level + 0.25 * s_rsi + 0.10 * s_band + 0.15 * s_turn;
        }
    }
    return out;
}

SignalOutcome evaluate_outcome(const std::vector<Bar>& bars, size_t index, double entry, SignalSide side,
                               double move_pct, int horizon) {
    const bool peak = side == SignalSide::Peak;
    const double target = peak ? entry * (1 - move_pct) : entry * (1 + move_pct);
    const double stop = peak ? entry * (1 + move_pct / 2) : entry * (1 - move_pct / 2);
    for (int k = 1; k <= horizon; ++k) {
        const size_t j = index + static_cast<size_t>(k);
        if (j >= bars.size()) return SignalOutcome::Pending;
        const Bar& b = bars[j];
        const double hi = b.high > 0 ? b.high : b.close;
        const double lo = b.low > 0 ? b.low : b.close;
        // The adverse barrier is checked first: if both are touched on one day we assume the worst.
        if (peak) {
            if (hi >= stop) return SignalOutcome::Wrong;
            if (lo <= target) return SignalOutcome::Correct;
        } else {
            if (lo <= stop) return SignalOutcome::Wrong;
            if (hi >= target) return SignalOutcome::Correct;
        }
    }
    return SignalOutcome::Expired;
}

}  // namespace strojanka::analysis
