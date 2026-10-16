#pragma once

#include "core/market/market_data.h"

namespace strojanka {

// Yahoo Finance public chart and search endpoints (no API key). Uses libcurl.
class YahooProvider : public MarketDataProvider {
public:
    YahooProvider();
    ~YahooProvider() override;
    std::optional<ChartData> fetch(const std::string& symbol, HistoryRange range, std::string* error) override;
    std::vector<SymbolMatch> search(const std::string& query) override;
};

// Exposed for unit tests: parses a /v8/finance/chart response body.
std::optional<ChartData> parse_yahoo_chart(const std::string& body, std::string* error);
// Exposed for unit tests: parses a /v1/finance/search response body (stocks and ETFs only).
std::vector<SymbolMatch> parse_yahoo_search(const std::string& body);

}  // namespace strojanka
