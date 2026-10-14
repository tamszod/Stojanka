#include "core/analysis/zigzag.h"

namespace strojanka::analysis {

ZigZag::ZigZag(double reversal_pct) : pct_(reversal_pct) {}

void ZigZag::update(size_t i, double c) {
    if (!started_) {
        started_ = true;
        min_ = max_ = extreme_ = c;
        min_index_ = max_index_ = extreme_index_ = i;
        return;
    }
    if (direction_ == 0) {
        if (c > max_) { max_ = c; max_index_ = i; }
        if (c < min_) { min_ = c; min_index_ = i; }
        if (c >= min_ * (1 + pct_) && min_index_ < i) {
            pivots_.push_back({min_index_, min_, false});
            direction_ = +1;
            extreme_ = max_;
            extreme_index_ = max_index_;
            if (extreme_index_ < min_index_) { extreme_ = c; extreme_index_ = i; }
        } else if (c <= max_ * (1 - pct_) && max_index_ < i) {
            pivots_.push_back({max_index_, max_, true});
            direction_ = -1;
            extreme_ = min_;
            extreme_index_ = min_index_;
            if (extreme_index_ < max_index_) { extreme_ = c; extreme_index_ = i; }
        }
        return;
    }
    if (direction_ == +1) {
        if (c > extreme_) {
            extreme_ = c;
            extreme_index_ = i;
        } else if (c <= extreme_ * (1 - pct_)) {
            pivots_.push_back({extreme_index_, extreme_, true});
            direction_ = -1;
            extreme_ = c;
            extreme_index_ = i;
        }
    } else {
        if (c < extreme_) {
            extreme_ = c;
            extreme_index_ = i;
        } else if (c >= extreme_ * (1 + pct_)) {
            pivots_.push_back({extreme_index_, extreme_, false});
            direction_ = +1;
            extreme_ = c;
            extreme_index_ = i;
        }
    }
}

std::vector<Pivot> zigzag_pivots(const std::vector<double>& closes, double reversal_pct) {
    ZigZag zz(reversal_pct);
    for (size_t i = 0; i < closes.size(); ++i) zz.update(i, closes[i]);
    return zz.pivots();
}

}  // namespace strojanka::analysis
