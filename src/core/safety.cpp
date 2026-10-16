#include "core/safety.h"

#include <algorithm>
#include <cmath>

#include "common/strings.h"

namespace strojanka {

SafetyLevels safety_levels(const Position& position, double fee, double daily_vol) {
    SafetyLevels l;
    if (!position.has_shares()) return l;
    l.cushion = std::clamp(2 * daily_vol, kMinCushion, kMaxCushion);
    l.floor = (position.cost_basis + std::max(0.0, fee)) / position.shares;
    l.warn = l.floor * (1 + l.cushion);
    l.rearm = l.floor * (1 + 2 * l.cushion);
    return l;
}

double sell_fee_from_setting(const std::string& stored) {
    const auto fee = parse_amount(stored);
    return fee && *fee >= 0 ? *fee : kDefaultSellFee;
}

SafetyAction safety_step(bool& armed, double price, bool falling, const SafetyLevels& levels) {
    if (price <= 0 || levels.floor <= 0) return SafetyAction::None;
    if (price >= levels.rearm) {
        armed = true;
        return SafetyAction::None;
    }
    if (!armed || !falling) return SafetyAction::None;
    if (price <= levels.floor) {
        armed = false;
        return SafetyAction::BelowFloor;
    }
    if (price <= levels.warn) {
        armed = false;
        return SafetyAction::Warn;
    }
    return SafetyAction::None;
}

double daily_volatility(const std::vector<Bar>& bars, size_t window) {
    const size_t n = std::min(bars.size(), window + 1);
    if (n < 11) return 0;
    std::vector<double> r;
    for (size_t i = bars.size() - n + 1; i < bars.size(); ++i) {
        if (bars[i - 1].close > 0 && bars[i].close > 0) r.push_back(std::log(bars[i].close / bars[i - 1].close));
    }
    if (r.size() < 10) return 0;
    double mean = 0;
    for (double x : r) mean += x;
    mean /= static_cast<double>(r.size());
    double var = 0;
    for (double x : r) var += (x - mean) * (x - mean);
    return std::sqrt(var / static_cast<double>(r.size() - 1));
}

}  // namespace strojanka
