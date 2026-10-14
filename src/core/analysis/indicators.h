#pragma once

#include <cstddef>
#include <vector>

#include "core/models.h"

namespace strojanka::analysis {

std::vector<double> closes_of(const std::vector<Bar>& bars);

// Wilder's RSI. Values before `period` bars are NaN.
std::vector<double> rsi(const std::vector<double>& closes, int period = 14);

// Bollinger %B: 0 = lower band, 1 = upper band. NaN before `period` bars.
std::vector<double> percent_b(const std::vector<double>& closes, int period = 20, double k = 2.0);

// Standard deviation of daily log returns over the last `lookback` returns.
double daily_volatility(const std::vector<double>& closes, size_t lookback = 252);

struct Regression {
    double slope = 0;  // per bar, on log prices
    double r2 = 0;
};
// Least squares fit of log(close) against time over closes[from, to).
Regression log_price_regression(const std::vector<double>& closes, size_t from, size_t to);

double median(std::vector<double> values);
double clamp01(double x);
// 0 at edge0, 1 at edge1, smooth in between. Works for edge0 > edge1 too (decreasing).
double smoothstep(double edge0, double edge1, double x);

}  // namespace strojanka::analysis
