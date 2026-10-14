#pragma once

#include <vector>

#include "core/analysis/analyzer.h"
#include "core/models.h"

namespace strojanka::analysis {

struct SideStats {
    int signals = 0;          // calls the parameters would have made in the backtest
    int correct = 0;
    double precision = 0;     // recency weighted and smoothed, 0..1
    double coverage = 0;      // share of real swings that got a call, 0..1
    double objective = 0;
};

struct CalibrationResult {
    AnalysisParams params;
    SideStats peak;
    SideStats support;
    double score = 0;
    bool enough_data = false;
};

// Starting parameters derived from the stock's own volatility.
AnalysisParams default_params(const std::vector<Bar>& bars);

// Walk-forward backtest over the stock's history: tries a grid of parameters, scores every
// call with evaluate_outcome() and keeps the most accurate set. Recent bars weigh more, so
// the parameters follow the stock as its behaviour changes. Live biases from `previous`
// (learned from real mistakes) are carried over.
CalibrationResult calibrate(const std::vector<Bar>& bars, const AnalysisParams& previous);

// Learning from live mistakes: given the latest resolved live calls of one side (newest
// first), raise the threshold bias when calls keep failing and relax it when they work.
double adjust_bias(double bias, const std::vector<SignalRecord>& recent_resolved);

}  // namespace strojanka::analysis
