#pragma once

#include <cstddef>
#include <vector>

#include "core/models.h"

namespace strojanka::analysis {

struct WalkForwardResult {
    int peak_signals = 0;
    int peak_correct = 0;
    int support_signals = 0;
    int support_correct = 0;
    int segments = 0;
    // Same grading applied to a call on every single day: what a random caller would score.
    int baseline_peak_days = 0;
    int baseline_peak_correct = 0;
    int baseline_support_days = 0;
    int baseline_support_correct = 0;
};

// Honest out-of-sample accuracy: calibrate on the past (at most `train_window` bars), make
// calls on the next `step` bars exactly as the live monitor would, grade them, move forward.
WalkForwardResult walk_forward(const std::vector<Bar>& bars, size_t train_window = 520, size_t step = 20);

}  // namespace strojanka::analysis
