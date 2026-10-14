#pragma once

#include <string>
#include <vector>

#include "core/models.h"

namespace strojanka::analysis {

// Behavioural categories, measured from price history (no sector data needed).
enum class StockCategory { Unknown, Cyclical, Uptrend, Downtrend, HighVolatility, Defensive, Mixed };

const char* category_name(StockCategory c);
const char* category_description(StockCategory c);

struct StockProfile {
    StockCategory category = StockCategory::Unknown;
    double daily_vol = 0;        // stdev of daily log returns
    double annual_vol = 0;       // daily_vol * sqrt(252)
    double annual_trend = 0;     // fitted yearly price change over the last year (0.2 = +20%/yr)
    double trend_r2 = 0;         // 0..1, how straight that trend line is
    double swings_per_year = 0;  // completed swing highs+lows per year
    int bars = 0;

    std::string to_json() const;
};

StockProfile classify(const std::vector<Bar>& bars);

enum class Trend { Increasing, Decreasing, Stagnating };
const char* trend_name(Trend t);
// Short-term (about one month) trend, scaled to the stock's own volatility.
Trend short_term_trend(const std::vector<double>& closes, double daily_vol, size_t window = 20);

}  // namespace strojanka::analysis
