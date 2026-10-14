#pragma once

#include <cstddef>
#include <vector>

namespace strojanka::analysis {

// A confirmed swing turning point.
struct Pivot {
    size_t index = 0;
    double price = 0;
    bool is_high = false;
};

// Online ZigZag: a swing high/low is only confirmed once price has reversed by
// `reversal_pct` from it, so it never looks into the future. The same class drives
// live analysis and backtests, which keeps both honest.
class ZigZag {
public:
    explicit ZigZag(double reversal_pct);

    void update(size_t index, double close);

    // +1: rising leg (last confirmed pivot was a low) - we are heading for a PEAK.
    // -1: falling leg (last confirmed pivot was a high) - we are heading for SUPPORT.
    //  0: not enough movement yet.
    int direction() const { return direction_; }
    const std::vector<Pivot>& pivots() const { return pivots_; }
    double extreme() const { return extreme_; }       // highest close of a rising leg / lowest of a falling leg
    size_t extreme_index() const { return extreme_index_; }

private:
    double pct_;
    int direction_ = 0;
    std::vector<Pivot> pivots_;
    double extreme_ = 0;
    size_t extreme_index_ = 0;
    double min_ = 0, max_ = 0;
    size_t min_index_ = 0, max_index_ = 0;
    bool started_ = false;
};

// All pivots confirmed over a full series (hindsight view).
std::vector<Pivot> zigzag_pivots(const std::vector<double>& closes, double reversal_pct);

}  // namespace strojanka::analysis
