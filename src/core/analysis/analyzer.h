#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/models.h"

namespace strojanka::analysis {

// Per-stock tunables. These are what the calibrator learns for every stock.
struct AnalysisParams {
    double swing_pct = 0.08;         // reversal needed to confirm a swing high/low
    double rsi_high = 68;            // RSI level that counts as fully "overbought"
    double rsi_low = 32;             // RSI level that counts as fully "oversold"
    double peak_threshold = 0.70;    // peak score needed for a PEAK call
    double support_threshold = 0.70; // support score needed for a SUPPORT call
    double peak_bias = 0;            // learned from live mistakes, added to the threshold
    double support_bias = 0;
    double move_pct = 0.05;          // price move that makes a call "correct"
    int horizon_bars = 20;           // trading days the move has to happen in

    double peak_cutoff() const;
    double support_cutoff() const;
    std::string to_json() const;
    static std::optional<AnalysisParams> from_json(const std::string& json);
};

struct Indicators {
    std::vector<double> rsi;
    std::vector<double> percent_b;
};
Indicators compute_indicators(const std::vector<double>& closes);

// Analysis result for one bar, using only data up to and including that bar.
struct BarState {
    bool valid = false;
    int direction = 0;            // +1 rising leg, -1 falling leg, 0 unknown
    double peak_score = 0;        // 0..1, how strongly this looks like the top of the rising leg
    double support_score = 0;     // 0..1, how strongly this looks like the bottom of the falling leg
    double support_level = 0;     // average of recent swing lows
    double resistance_level = 0;  // average of recent swing highs
    double leg_origin_price = 0;  // price of the swing low/high where the current leg began
    double rsi = 50;
    double percent_b = 0.5;
    size_t leg_start = 0;         // bar index of the pivot that started the current leg
};

std::vector<BarState> analyze_series(const std::vector<double>& closes, const Indicators& indicators,
                                     const AnalysisParams& params);
inline std::vector<BarState> analyze_series(const std::vector<double>& closes, const AnalysisParams& params) {
    return analyze_series(closes, compute_indicators(closes), params);
}

// "Triple barrier" check for a call made at bars[index] with price `entry`:
//   PEAK is correct if price falls move_pct before it rises move_pct/2 (SUPPORT mirrored),
//   Wrong if the opposite barrier is hit first, Expired if neither happens within `horizon`
//   bars, Pending if the data runs out first.
SignalOutcome evaluate_outcome(const std::vector<Bar>& bars, size_t index, double entry, SignalSide side,
                               double move_pct, int horizon);

}  // namespace strojanka::analysis
