#pragma once

#include <string>
#include <vector>

#include "core/models.h"
#include "core/portfolio.h"

namespace strojanka {

// Safety alerts: a holding that has been in profit and then falls back toward what it cost gets
// one warning while selling it would still make money after the sale fee.
//
//   floor = (what the shares cost + the fee) / shares   selling below this loses money
//   warn  = floor x (1 + cushion)                       the warning comes when the price falls to here
//   rearm = floor x (1 + 2 x cushion)                   it must climb back up here to warn again
//
// The cushion is about two normal days' moves of the stock (2 x its daily volatility), kept
// between 1.5% and 8%: a calm stock is warned close to the line, a jumpy one earlier. A fall that
// jumps past the whole zone between two checks gets a "fell below what you paid" message instead.
// Either message only goes out while the price is falling (lower than at the previous check): a
// stock in the zone that is climbing again stays quiet, and is warned if it turns down again.
constexpr double kDefaultSellFee = 0.20;  // per sale, in the stock's currency
constexpr double kMinCushion = 0.015;
constexpr double kMaxCushion = 0.08;

// User settings keys (Repository::user_setting).
constexpr const char* kSafetySettingKey = "safety";  // "off" = no safety alerts
constexpr const char* kSellFeeSettingKey = "sell_fee";
// "safety:NVDA" = "off": no safety alerts for that one stock.
inline std::string safety_stock_key(const std::string& symbol) { return std::string(kSafetySettingKey) + ":" + symbol; }

struct SafetyLevels {
    double floor = 0;
    double warn = 0;
    double rearm = 0;
    double cushion = 0;
};
SafetyLevels safety_levels(const Position& position, double fee, double daily_vol);
// The user's stored sell_fee setting, or kDefaultSellFee when it's empty or not a number.
double sell_fee_from_setting(const std::string& stored);

enum class SafetyAction { None, Warn, BelowFloor };
// One price check. `armed` is updated: set once the price is at or above the re-arm line,
// cleared when a warning is sent. falling: the price is lower than at the previous check.
SafetyAction safety_step(bool& armed, double price, bool falling, const SafetyLevels& levels);

// Standard deviation of daily log returns over the last `window` bars (0 with fewer than 10).
double daily_volatility(const std::vector<Bar>& bars, size_t window = 60);

}  // namespace strojanka
