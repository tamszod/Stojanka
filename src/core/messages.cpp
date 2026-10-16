#include "core/messages.h"

#include <cmath>
#include <sstream>

#include "common/strings.h"
#include "core/monitor_service.h"
#include "core/ml/pythia.h"
#include "core/news/sentiment.h"

namespace strojanka {

namespace {

constexpr int kMinCallsForEdge = 8;
constexpr double kEdgeMargin = 0.05;

std::string track_record(const char* label, int correct, int signals, double baseline) {
    std::ostringstream m;
    m << label << ": ";
    if (signals == 0) {
        m << "no calls in the test yet";
    } else {
        m << correct << " of " << signals << " right (" << format_percent(static_cast<double>(correct) / signals, 0)
          << ") vs " << format_percent(baseline, 0) << " for a call on a random day";
        if (!has_proven_edge(correct, signals, baseline)) m << " - no proven edge yet";
    }
    return m.str();
}

std::string alert_evidence(int correct, int signals, double baseline) {
    if (signals == 0) return "No track record for this stock yet.";
    std::string s = "Past record for this call: " + std::to_string(correct) + " of " + std::to_string(signals) +
                    " right (random day: " + format_percent(baseline, 0) + ").";
    if (!has_proven_edge(correct, signals, baseline)) s += " Treat it as a hint, not a certainty.";
    return s;
}

}  // namespace

bool has_proven_edge(int correct, int signals, double baseline) {
    return signals >= kMinCallsForEdge && static_cast<double>(correct) / signals >= baseline + kEdgeMargin;
}

std::string peak_alert(const std::string& symbol, double shares, double price, const std::string& currency,
                       double resistance, double strength, const StockParamsRecord& learning) {
    std::ostringstream m;
    m << "**" << symbol << "** has reached PEAK. You have " << format_shares(shares) << " shares. It is time to sell.\n"
      << "Price " << format_money(price, currency) << " (resistance around " << format_money(resistance, currency)
      << "), signal strength " << format_percent(strength, 0) << ".\n"
      << alert_evidence(learning.backtest_peak_correct, learning.backtest_peak_signals, learning.baseline_peak);
    return m.str();
}

std::string support_alert(const std::string& symbol, double price, const std::string& currency, double support,
                          double strength, const StockParamsRecord& learning) {
    std::ostringstream m;
    m << "**" << symbol << "** has reached SUPPORT level. It is time to buy.\n"
      << "Price " << format_money(price, currency) << " (support around " << format_money(support, currency)
      << "), signal strength " << format_percent(strength, 0) << ".\n"
      << alert_evidence(learning.backtest_support_correct, learning.backtest_support_signals, learning.baseline_support);
    return m.str();
}

std::string format_report(const StockReport& r) {
    using namespace analysis;
    const auto& s = r.stock;
    const auto& l = r.learning;
    std::ostringstream m;
    m << "**" << s.symbol << "**";
    if (!s.name.empty() && s.name != s.symbol) m << " - " << s.name;
    if (!s.exchange.empty()) m << " (" << s.exchange << (r.is_fund() ? ", ETF" : "") << ")";
    else if (r.is_fund()) m << " (ETF)";
    m << "\n";
    m << "Price: " << format_money(s.last_price, s.currency) << " | trend: " << s.trend << "\n";
    m << "Category: " << category_name(r.profile.category) << "\n> " << category_description(r.profile.category) << "\n";
    m << "Volatility: " << format_percent(r.profile.annual_vol, 0) << " per year | 1-year trend: "
      << (r.profile.annual_trend >= 0 ? "+" : "") << format_percent(r.profile.annual_trend, 0)
      << " | swings per year: " << format_double(r.profile.swings_per_year, 1) << "\n";

    if (r.state.valid) {
        m << "Support: " << format_money(s.support_level, s.currency)
          << " | Resistance: " << format_money(s.resistance_level, s.currency) << "\n";
        if (r.state.direction > 0) {
            m << "Cycle: rising since the swing low of " << format_money(r.state.leg_origin_price, s.currency)
              << ". Peak score " << format_percent(r.peak_score, 0) << " (alert at "
              << format_percent(r.params.peak_cutoff(), 0) << ").\n";
        } else {
            m << "Cycle: falling since the swing high of " << format_money(r.state.leg_origin_price, s.currency)
              << ". Support score " << format_percent(r.support_score, 0) << " (alert at "
              << format_percent(r.params.support_cutoff(), 0) << ").\n";
        }
    } else {
        m << "Cycle: not enough movement yet to identify swing highs and lows.\n";
    }

    m << "Learned settings: swing " << format_percent(r.params.swing_pct, 1) << "; a call counts as right after a "
      << format_percent(r.params.move_pct, 1) << " move within " << r.params.horizon_bars << " trading days.\n";
    m << "Honest test (re-learned monthly on past data, calls made blind):\n";
    m << "- " << track_record("PEAK calls", l.backtest_peak_correct, l.backtest_peak_signals, l.baseline_peak) << "\n";
    m << "- " << track_record("SUPPORT calls", l.backtest_support_correct, l.backtest_support_signals, l.baseline_support) << "\n";
    if (r.pythia.valid) m << pythia_line(r.pythia) << "\n";
    if (r.is_fund()) m << "News: not used for ETFs (a fund moves with its whole basket, not with headlines about it).";
    else m << news_alert_line(r.news);
    if (r.news.headlines > 0 && r.state.valid) {
        const double moved = (r.state.direction > 0 ? r.peak_score - r.state.peak_score : r.support_score - r.state.support_score);
        if (std::fabs(moved) >= 0.005) {
            m << " It moved the " << (r.state.direction > 0 ? "peak" : "support") << " score by "
              << (moved > 0 ? "+" : "") << format_double(moved * 100, 0) << " points.";
        }
    }
    m << "\nHistory: " << r.bars << " trading days analysed (" << r.first_bar_date << " to " << r.last_bar_date << ").";
    return m.str();
}

std::string pythia_line(const ml::Prediction& p) {
    if (!p.valid) return "Pythia (AI): not trained yet.";
    return "Pythia (AI): " + format_percent(p.up, 0) + " chance of a +" + format_percent(p.move, 1) + " rise before a -" +
           format_percent(p.move / 2, 1) + " dip within 20 trading days (" + format_percent(p.base_up, 0) +
           " on an average day); " + format_percent(p.down, 0) + " chance of the opposite (" + format_percent(p.base_down, 0) + ").";
}

std::string news_alert_line(const news::NewsSummary& n) {
    if (n.headlines == 0) return "News: no headlines in the last 3 days.";
    std::string line = "News: " + news::describe(n.sentiment) + " (" + std::to_string(n.headlines) + " headline" +
                       (n.headlines == 1 ? "" : "s") + " in 3 days).";
    if (!n.red_flags.empty()) line += " **Red flag:** " + n.red_flags.front();
    return line;
}

std::string format_news(const std::string& symbol, const std::vector<NewsItem>& recent, const news::NewsSummary& n,
                        double weight) {
    std::ostringstream m;
    m << "**" << symbol << " news**\n";
    if (recent.empty()) {
        m << "No headlines found in the last two weeks.";
        return m.str();
    }
    for (const auto& item : recent) {
        const char* mark = item.red_flag ? "\xE2\x9B\x94" : item.sentiment > 0.08 ? "\xF0\x9F\x9F\xA2"
                                                          : item.sentiment < -0.08 ? "\xF0\x9F\x94\xB4"
                                                                                   : "\xE2\x9A\xAA";
        m << mark << " " << item.published_at.substr(0, 10) << " " << item.title;
        if (!item.source.empty()) m << " *(" << item.source << ")*";
        m << "\n";
    }
    m << "\n" << news_alert_line(n) << "\n"
      << "News moves PEAK/SUPPORT scores by up to " << format_double(weight * 100, 0)
      << " points. This weight is learned from graded alerts: it grows if news helped and shrinks to 0 if it did "
         "not. Red-flag news (bankruptcy, fraud, regulator probe...) blocks buy alerts.\n"
      << "*Tone is read from headlines with a finance word list, so it is rough.*";
    return m.str();
}

}  // namespace strojanka
