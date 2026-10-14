#include "core/analysis/classifier.h"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "core/analysis/indicators.h"
#include "core/analysis/zigzag.h"

namespace strojanka::analysis {

const char* category_name(StockCategory c) {
    switch (c) {
        case StockCategory::Cyclical: return "Cyclical / range-bound";
        case StockCategory::Uptrend: return "Trending up";
        case StockCategory::Downtrend: return "Trending down";
        case StockCategory::HighVolatility: return "Speculative / high volatility";
        case StockCategory::Defensive: return "Defensive / low volatility";
        case StockCategory::Mixed: return "Mixed";
        case StockCategory::Unknown: break;
    }
    return "Unknown (not enough history)";
}

const char* category_description(StockCategory c) {
    switch (c) {
        case StockCategory::Cyclical:
            return "Swings up and down between fairly stable levels. PEAK and SUPPORT calls fit this kind of stock best.";
        case StockCategory::Uptrend:
            return "Has climbed steadily over the past year. Prices that looked 'too high' often kept rising, so be careful with PEAK calls.";
        case StockCategory::Downtrend:
            return "Has fallen steadily over the past year. Prices that looked 'cheap' often kept falling, so be careful with SUPPORT calls.";
        case StockCategory::HighVolatility:
            return "Large, fast moves. Levels break often; treat every call with extra caution.";
        case StockCategory::Defensive:
            return "Moves slowly. Swings are small, so calls are rarer and profits per swing are smaller.";
        case StockCategory::Mixed:
            return "No single dominant behaviour yet. The bot keeps learning as more history arrives.";
        case StockCategory::Unknown: break;
    }
    return "Fewer than six months of price history is available.";
}

std::string StockProfile::to_json() const {
    nlohmann::json j = {
        {"category", category_name(category)}, {"daily_vol", daily_vol},     {"annual_vol", annual_vol},
        {"annual_trend", annual_trend},        {"trend_r2", trend_r2},       {"swings_per_year", swings_per_year},
        {"bars", bars},
    };
    return j.dump();
}

StockProfile classify(const std::vector<Bar>& bars) {
    StockProfile p;
    p.bars = static_cast<int>(bars.size());
    const auto closes = closes_of(bars);
    p.daily_vol = daily_volatility(closes, 252);
    p.annual_vol = p.daily_vol * std::sqrt(252.0);
    if (bars.size() < 120) return p;

    const size_t n = closes.size();
    const size_t from = n > 252 ? n - 252 : 0;
    const auto reg = log_price_regression(closes, from, n);
    p.annual_trend = std::exp(reg.slope * 252) - 1;
    p.trend_r2 = reg.r2;

    const double swing = std::clamp(4.0 * p.daily_vol, 0.03, 0.30);
    size_t swings = 0;
    for (const auto& pv : zigzag_pivots(closes, swing)) {
        if (pv.index >= from) ++swings;
    }
    p.swings_per_year = static_cast<double>(swings) * 252.0 / static_cast<double>(n - from);

    if (p.annual_vol > 0.60) p.category = StockCategory::HighVolatility;
    else if (p.annual_trend > 0.20 && p.trend_r2 > 0.5) p.category = StockCategory::Uptrend;
    else if (p.annual_trend < -0.20 && p.trend_r2 > 0.5) p.category = StockCategory::Downtrend;
    else if (p.annual_vol < 0.18) p.category = StockCategory::Defensive;
    else if (p.swings_per_year >= 4 && p.trend_r2 < 0.5) p.category = StockCategory::Cyclical;
    else p.category = StockCategory::Mixed;
    return p;
}

const char* trend_name(Trend t) {
    switch (t) {
        case Trend::Increasing: return "increasing";
        case Trend::Decreasing: return "decreasing";
        case Trend::Stagnating: return "stagnating";
    }
    return "stagnating";
}

Trend short_term_trend(const std::vector<double>& closes, double daily_vol, size_t window) {
    if (closes.size() < window + 1) return Trend::Stagnating;
    const auto reg = log_price_regression(closes, closes.size() - window, closes.size());
    const double change = std::exp(reg.slope * static_cast<double>(window)) - 1;
    const double threshold = std::max(0.015, 0.35 * daily_vol * std::sqrt(static_cast<double>(window)));
    if (change > threshold) return Trend::Increasing;
    if (change < -threshold) return Trend::Decreasing;
    return Trend::Stagnating;
}

}  // namespace strojanka::analysis
