#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/models.h"

namespace strojanka {

struct ChartData {
    std::string symbol;
    std::string name;
    std::string currency = "USD";
    std::string exchange;
    double last_price = 0;
    std::vector<Bar> bars;  // daily, oldest first, split/dividend adjusted
};

enum class HistoryRange { Month, TwoYears, TenYears };

// Source of price data. Implementations must be safe to call from several threads at once.
class MarketDataProvider {
public:
    virtual ~MarketDataProvider() = default;
    virtual std::optional<ChartData> fetch(const std::string& symbol, HistoryRange range, std::string* error) = 0;
};

}  // namespace strojanka
