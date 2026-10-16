#pragma once

#include <string>
#include <vector>

#include "core/models.h"
#include "core/safety.h"

namespace strojanka {

struct StockReport;

// True when the walk-forward record beats calling on a random day by a clear margin.
bool has_proven_edge(int correct, int signals, double baseline);

// Alert texts. The first sentence of each follows the wording agreed in the task spec;
// the following lines give the evidence, including the honest historical hit rate.
// The alerts name the stock as "**FCX** (Freeport-McMoRan)": `company` is its full name, shortened here.
std::string peak_alert(const std::string& symbol, const std::string& company, double shares, double price,
                       const std::string& currency, double resistance, double strength, const StockParamsRecord& learning);
std::string support_alert(const std::string& symbol, const std::string& company, double price, const std::string& currency,
                          double support, double strength, const StockParamsRecord& learning);

// "**FCX** (Freeport-McMoRan)": the ticker, plus the short company name when it says more.
std::string stock_label(const std::string& symbol, const std::string& company);

// A /watch price alert that was reached. `held` = the user's position in it (nullptr if none).
std::string price_alert_message(const PriceAlertRecord& alert, const std::string& company, double price,
                                const std::string& currency, const Position* held);
// Safety alert for a holding falling toward what it cost (below = it already fell past the floor).
std::string safety_alert(const std::string& symbol, const std::string& company, const Position& position, double price,
                         const std::string& currency, const SafetyLevels& levels, double fee, bool below);

// Multi-line analysis report shown by /monitor and /report.
std::string format_report(const StockReport& report);

namespace news {
struct NewsSummary;
}
namespace ml {
struct Prediction;
}
// "Pythia (AI): 58% chance of a +7.4% rise before a -3.7% dip within 20 trading days (38% on an average day)..."
std::string pythia_line(const ml::Prediction& p);

// One line for alerts, e.g. "News: slightly negative (7 headlines in 3 days)."
std::string news_alert_line(const news::NewsSummary& summary);
// The `news` command: recent headlines with their tone, the 3-day summary and how news is weighted.
std::string format_news(const std::string& symbol, const std::vector<NewsItem>& recent, const news::NewsSummary& summary,
                        double weight);

}  // namespace strojanka
