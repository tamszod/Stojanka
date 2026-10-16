#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/models.h"

namespace strojanka {

struct ChartData {
    std::string symbol;
    std::string name;
    std::string currency = "USD";  // always a major unit (pence-quoted prices are converted to pounds)
    std::string exchange;
    std::string instrument_type;   // "EQUITY", "ETF", "MUTUALFUND", ... as the data source names it
    double last_price = 0;
    std::vector<Bar> bars;  // daily, oldest first, split/dividend adjusted

    bool is_fund() const { return instrument_type == "ETF" || instrument_type == "MUTUALFUND"; }
};

enum class HistoryRange { Month, TwoYears, TenYears };

// One result of a symbol search ("CSPX" -> CSPX.L on the London Stock Exchange).
struct SymbolMatch {
    std::string symbol;
    std::string name;
    std::string type;      // "EQUITY" or "ETF"
    std::string exchange;  // readable, e.g. "London"
};

// Source of price data. Implementations must be safe to call from several threads at once.
class MarketDataProvider {
public:
    virtual ~MarketDataProvider() = default;
    virtual std::optional<ChartData> fetch(const std::string& symbol, HistoryRange range, std::string* error) = 0;
    // Stocks and ETFs matching a ticker or a name, best first. Empty when unsupported or nothing matches.
    virtual std::vector<SymbolMatch> search(const std::string& query) {
        (void)query;
        return {};
    }
};

// Some exchanges quote in a minor unit (London: GBp pence). Returns the major currency and the
// divisor to get there ("GBp" -> "GBP", 100), or the currency unchanged with 1.
std::pair<std::string, double> major_currency(const std::string& currency);

}  // namespace strojanka
