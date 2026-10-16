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

// json::value() throws when the key exists but holds null, which Yahoo does for empty listings
// (OTP: "currency": null), so read fields only when they have the expected type.
std::string text_or(const nlohmann::json& obj, const char* key, const std::string& fallback) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : fallback;
}

double number_or(const nlohmann::json& obj, const char* key, double fallback) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_number() ? it->get<double>() : fallback;
}

}  // namespace

std::pair<std::string, double> major_currency(const std::string& currency) {
    if (currency == "GBp" || currency == "GBX") return {"GBP", 100.0};
    if (currency == "ZAc") return {"ZAR", 100.0};
    if (currency == "ILA") return {"ILS", 100.0};
    return {currency, 1.0};
}

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
    if (data && data->bars.empty() && data->last_price <= 0) {
        // Listed but never priced (plain "OTP" is an empty US fund; the bank is OTP.BD), so
        // report it like an unknown ticker and let the caller suggest the real listings.
        if (error) *error = "symbol not found (Yahoo lists " + symbol + " but has no prices for it)";
        return std::nullopt;
    }
    return data;
}

std::vector<SymbolMatch> YahooProvider::search(const std::string& query) {
    const HttpResponse res = http_get("https://query2.finance.yahoo.com/v1/finance/search?q=" + url_encode(query) +
                                      "&quotesCount=10&newsCount=0&listsCount=0");
    if (!res.ok || res.status != 200) return {};
    return parse_yahoo_search(res.body);
}

std::vector<SymbolMatch> parse_yahoo_search(const std::string& body) {
    std::vector<SymbolMatch> out;
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("quotes") || !j["quotes"].is_array()) return out;
    for (const auto& q : j["quotes"]) {
        if (!q.is_object()) continue;
        const std::string type = q.value("quoteType", std::string());
        const std::string symbol = q.value("symbol", std::string());
        if ((type != "EQUITY" && type != "ETF") || symbol.empty()) continue;
        SymbolMatch m;
        m.symbol = symbol;
        m.type = type;
        m.exchange = q.value("exchDisp", q.value("exchange", std::string()));
        m.name = q.contains("longname") && q["longname"].is_string() ? q["longname"].get<std::string>()
                                                                       : q.value("shortname", std::string());
        out.push_back(std::move(m));
    }
    return out;
}

std::optional<ChartData> parse_yahoo_chart(const std::string& body, std::string* error) {
    try {
        const auto j = nlohmann::json::parse(body);
        const auto& chart = j.at("chart");
        if (chart.contains("error") && !chart["error"].is_null()) {
            if (error) *error = chart["error"].is_object() ? text_or(chart["error"], "description", "symbol not found") : "symbol not found";
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
        d.symbol = text_or(meta, "symbol", "");
        const auto [currency, divisor] = major_currency(text_or(meta, "currency", "USD"));
        d.currency = currency;
        d.exchange = text_or(meta, "fullExchangeName", text_or(meta, "exchangeName", ""));
        d.instrument_type = text_or(meta, "instrumentType", "");
        d.name = text_or(meta, "longName", text_or(meta, "shortName", d.symbol));
        d.last_price = number_or(meta, "regularMarketPrice", 0.0) / divisor;
        const auto gmtoffset = static_cast<int64_t>(number_or(meta, "gmtoffset", 0.0));

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
            b.close = close * factor / divisor;
            b.open = field(opens) / divisor;
            b.high = field(highs) / divisor;
            b.low = field(lows) / divisor;
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
