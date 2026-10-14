#include "core/market/yahoo_provider.h"

#include <cmath>

#include <nlohmann/json.hpp>

#include "common/strings.h"
#include "core/http.h"

namespace strojanka {

namespace {

double number_or_nan(const nlohmann::json& arr, size_t i) {
    if (!arr.is_array() || i >= arr.size() || !arr[i].is_number()) return std::nan("");
    return arr[i].get<double>();
}

}  // namespace

YahooProvider::YahooProvider() = default;

YahooProvider::~YahooProvider() = default;

std::optional<ChartData> YahooProvider::fetch(const std::string& symbol, HistoryRange range, std::string* error) {
    const std::string url = "https://query1.finance.yahoo.com/v8/finance/chart/" + url_encode(symbol) +
                            "?interval=1d&includePrePost=false&events=div%2Csplit&range=" +
                            (range == HistoryRange::TenYears   ? "10y"
                             : range == HistoryRange::TwoYears ? "2y"
                                                               : "1mo");
    const HttpResponse res = http_get(url);
    if (!res.ok) {
        if (error) *error = res.error;
        return std::nullopt;
    }
    if (res.status == 404) {
        if (error) *error = "symbol not found";
        return std::nullopt;
    }
    if (res.status != 200) {
        if (error) *error = "market data server answered HTTP " + std::to_string(res.status);
        return std::nullopt;
    }
    auto data = parse_yahoo_chart(res.body, error);
    if (data && data->symbol.empty()) data->symbol = symbol;
    return data;
}

std::optional<ChartData> parse_yahoo_chart(const std::string& body, std::string* error) {
    try {
        const auto j = nlohmann::json::parse(body);
        const auto& chart = j.at("chart");
        if (chart.contains("error") && !chart["error"].is_null()) {
            if (error) *error = chart["error"].value("description", std::string("symbol not found"));
            return std::nullopt;
        }
        const auto& results = chart.at("result");
        if (!results.is_array() || results.empty()) {
            if (error) *error = "symbol not found";
            return std::nullopt;
        }
        const auto& r = results[0];
        const auto& meta = r.at("meta");
        ChartData d;
        d.symbol = meta.value("symbol", std::string());
        d.currency = meta.value("currency", std::string("USD"));
        d.exchange = meta.value("fullExchangeName", meta.value("exchangeName", std::string()));
        d.name = meta.value("longName", meta.value("shortName", d.symbol));
        d.last_price = meta.value("regularMarketPrice", 0.0);
        const int64_t gmtoffset = meta.value("gmtoffset", static_cast<int64_t>(0));

        if (!r.contains("timestamp") || !r["timestamp"].is_array()) return d;  // listed but no trading yet
        const auto& ts = r["timestamp"];
        const auto& quote = r.at("indicators").at("quote").at(0);
        static const nlohmann::json kEmpty;
        auto column = [](const nlohmann::json& obj, const char* name) -> const nlohmann::json& {
            return obj.is_object() && obj.contains(name) ? obj[name] : kEmpty;
        };
        const auto& closes = column(quote, "close");
        const auto& opens = column(quote, "open");
        const auto& highs = column(quote, "high");
        const auto& lows = column(quote, "low");
        const auto& volumes = column(quote, "volume");
        const auto& adj_list = column(r["indicators"], "adjclose");
        const auto& adj = adj_list.is_array() && !adj_list.empty() ? column(adj_list[0], "adjclose") : kEmpty;

        for (size_t i = 0; i < ts.size(); ++i) {
            const double close = number_or_nan(closes, i);
            if (std::isnan(close) || close <= 0) continue;
            double factor = 1.0;
            const double adjusted = number_or_nan(adj, i);
            if (!std::isnan(adjusted) && adjusted > 0) factor = adjusted / close;
            auto field = [&](const nlohmann::json& arr) {
                const double v = number_or_nan(arr, i);
                return std::isnan(v) ? close * factor : v * factor;
            };
            Bar b;
            b.date = unix_to_date(ts[i].get<int64_t>() + gmtoffset);
            b.close = close * factor;
            b.open = field(opens);
            b.high = field(highs);
            b.low = field(lows);
            const double vol = number_or_nan(volumes, i);
            b.volume = std::isnan(vol) ? 0 : vol;
            if (!d.bars.empty() && d.bars.back().date == b.date) d.bars.back() = b;
            else d.bars.push_back(b);
        }
        if (d.last_price <= 0 && !d.bars.empty()) d.last_price = d.bars.back().close;
        return d;
    } catch (const std::exception& e) {
        if (error) *error = std::string("unexpected market data format: ") + e.what();
        return std::nullopt;
    }
}

}  // namespace strojanka
