#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "common/config.h"
#include "core/analysis/analyzer.h"
#include "core/analysis/calibrator.h"
#include "core/analysis/classifier.h"
#include "core/market/market_data.h"
#include "core/markets.h"
#include "core/ml/pythia.h"
#include "core/news/news_provider.h"
#include "core/news/sentiment.h"
#include "core/repository.h"

namespace strojanka {

// Delivers alert messages to a user. The Discord bot implements this; keeping it abstract
// lets the monitor run without Discord (tests, the --analyze command, future front-ends).
class Notifier {
public:
    virtual ~Notifier() = default;
    // Must report the result through Repository::mark_notification(notification_id, ...).
    virtual void send(int64_t notification_id, const std::string& user_id, const std::string& message) = 0;
    // A message not tied to a stock alert (admin warnings). Best effort, nothing is recorded.
    virtual void send_direct(const std::string& user_id, const std::string& message) {
        (void)user_id;
        (void)message;
    }
};

// Outcome of the latest monitoring cycle, for the admin panel and outage warnings.
struct CycleStats {
    std::string finished_at;
    size_t refreshed = 0;
    size_t total = 0;
    int failed_cycles_in_a_row = 0;  // cycles where not a single stock could be refreshed
    std::string last_error;
    // Stocks not checked because their market is closed (their closing prices are in already).
    size_t skipped_closed = 0;
    bool all_closed = false;           // every watched stock's market is closed: nothing is being checked
    int64_t next_open_at = 0;          // when the first of those markets opens (unix), 0 = none skipped
    std::string next_open_market;
};

// Everything known about one stock after a refresh.
struct StockReport {
    StockRecord stock;
    analysis::StockProfile profile;
    analysis::BarState state;
    analysis::AnalysisParams params;
    StockParamsRecord learning;  // backtest accuracy of the current parameters
    int bars = 0;
    std::string first_bar_date;
    std::string last_bar_date;
    news::NewsSummary news;  // headlines from the last three days
    double news_weight = 0;  // how strongly news moved the scores
    double peak_score = 0;   // state.peak_score adjusted for news
    double support_score = 0;
    ml::Prediction pythia;   // the AI model's view (valid once it has been trained)
    std::string instrument_type;  // "EQUITY", "ETF", ... (empty if the data source didn't say)

    bool is_fund() const { return instrument_type == "ETF" || instrument_type == "MUTUALFUND"; }
};

class MonitorService {
public:
    // `news` is optional; without it no headlines are fetched (stored ones are still used).
    MonitorService(Repository& repo, MarketDataProvider& provider, const Config& config,
                   news::NewsProvider* news = nullptr);
    ~MonitorService();

    void set_notifier(Notifier* notifier) { notifier_ = notifier; }
    void set_pythia(ml::Pythia* pythia) { pythia_ = pythia; }

    // Background loop: one cycle right away, then one every poll_interval_minutes.
    void start();
    void stop();

    // Refreshes every watched symbol (deduplicated across users) in parallel. Stocks whose market is
    // closed and whose closing prices are already in are skipped, unless `force`.
    void run_cycle(bool force = false, int64_t now = 0);  // now: 0 = the current time (tests pass one)
    // Recomputes which markets are closed and when checking starts again, right away rather than at
    // the next cycle: call after a stock is added or removed, so a market that opens sooner counts.
    void update_schedule(int64_t now = 0);

    // Fetches prices, resolves old calls, re-learns parameters when due, analyses the latest
    // bar and (if emit_signals) sends PEAK/SUPPORT alerts. Safe to call from any thread.
    std::optional<StockReport> refresh(const std::string& symbol, bool emit_signals, std::string* error);

    // Stocks and ETFs matching a ticker or name, for "did you mean" replies.
    std::vector<SymbolMatch> search_symbols(const std::string& query) { return provider_.search(query); }
    // The same ticker on the main exchanges (HEAL -> HEAL.L, HEAL.DE, ...): only listings that exist.
    // Symbol search often misses European listings of a ticker that also trades in the US.
    std::vector<SymbolMatch> probe_listings(const std::string& base);

    // Current weight of news in the scores (learned, starts at news::kDefaultNewsWeight).
    double news_weight();
    // Re-learns the news weight when new graded alerts exist. Called after each cycle.
    void learn_news_weight();

    CycleStats last_cycle();

private:
    struct Plan {
        std::vector<std::string> symbols;  // to fetch now
        size_t closed = 0;                 // skipped: market closed, closing prices already in
        const Market* opens_first = nullptr;
        int64_t opens_at = 0;
    };
    Plan plan_cycle(bool force, int64_t now);
    bool record_plan(const Plan& plan);  // returns whether every market was closed before
    bool wait_for_next_cycle();          // false when stopping
    std::mutex& symbol_mutex(const std::string& symbol);
    void emit(const std::string& symbol, const StockReport& report, SignalSide side, double score, double cutoff,
              const std::string& leg_key, double signal_price);

    Repository& repo_;
    MarketDataProvider& provider_;
    Config config_;
    news::NewsProvider* news_;
    ml::Pythia* pythia_ = nullptr;
    Notifier* notifier_ = nullptr;

    std::mutex stats_mutex_;
    CycleStats stats_;
    std::mutex symbols_mutex_;
    std::map<std::string, std::unique_ptr<std::mutex>> symbol_mutexes_;

    std::thread thread_;
    std::mutex loop_mutex_;
    std::condition_variable loop_cv_;
    bool stopping_ = false;
    bool reschedule_ = false;
};

}  // namespace strojanka
