#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace strojanka {

// One daily candle. Prices are split/dividend adjusted so history stays comparable.
struct Bar {
    std::string date;  // YYYY-MM-DD (exchange local date)
    double open = 0, high = 0, low = 0, close = 0, volume = 0;
};

enum class TxSide { Buy, Sell };

struct TransactionRecord {
    int64_t id = 0;
    std::string user_id;
    std::string symbol;
    TxSide side = TxSide::Buy;
    double shares = 0;
    double total = 0;  // total money paid (BUY) or received (SELL)
    std::string created_at;
};

struct StockRecord {
    std::string symbol;
    std::string name;
    std::string currency = "USD";
    std::string exchange;
    std::string category;
    std::string trend;
    double last_price = 0;
    std::string last_price_at;
    double support_level = 0;
    double resistance_level = 0;
    double peak_score = 0;
    double support_score = 0;
    std::string profile_json;
    std::string history_full_refresh_at;
    std::string news_fetched_at;
};

enum class SignalSide { Peak, Support };
enum class SignalOutcome { Pending, Correct, Wrong, Expired };

const char* to_string(SignalSide side);
const char* to_string(SignalOutcome outcome);
SignalSide signal_side_from_string(const std::string& s);
SignalOutcome signal_outcome_from_string(const std::string& s);

struct SignalRecord {
    int64_t id = 0;
    std::string symbol;
    SignalSide side = SignalSide::Peak;
    std::string bar_date;
    double price = 0;
    double score = 0;
    double threshold = 0;
    std::string leg_key;  // identifies the price swing; one signal per side per swing
    double move_pct = 0;  // move needed for the call to count as correct
    int horizon_bars = 0; // trading days allowed for that move
    std::string params_json;
    std::optional<double> news_sentiment;  // news tone when the alert fired (none if no headlines)
    SignalOutcome outcome = SignalOutcome::Pending;
    std::string created_at;
    std::string resolved_at;
};

// Learned parameters plus an honest accuracy record. The backtest_* numbers come from a
// walk-forward test (re-learn on the past, call the future blind), never from the data the
// parameters were fitted on. baseline_* is the hit rate of making the same call every day.
struct StockParamsRecord {
    std::string symbol;
    std::string params_json;
    double calibration_score = 0;
    std::string calibrated_at;
    int calibration_count = 0;
    int backtest_peak_signals = 0;
    int backtest_peak_correct = 0;
    int backtest_support_signals = 0;
    int backtest_support_correct = 0;
    double baseline_peak = 0;
    double baseline_support = 0;
    std::string backtest_at;
};

// One news headline about a stock, with its tone score.
struct NewsItem {
    std::string symbol;
    std::string title;
    std::string link;
    std::string source;       // publisher, e.g. "Reuters"
    std::string published_at; // ISO UTC
    double sentiment = 0;     // -1 .. +1
    bool red_flag = false;
    std::string title_key;    // dedup key, see news::title_key
};

// Arena bot (paper trading). stage: "benchmark", "training", "proving" (live, private) or "public".
struct ArenaBotRecord {
    std::string name;
    std::string stage = "training";
    double cash = 1000;
    double peak_value = 1000;
    std::string last_date;      // last trading day processed live
    std::string live_started;   // first live trading day
    std::string stage_changed;  // trading day the current stage began
    std::string backtest_json;
    std::string backtested_at;
};

struct ArenaPositionRecord {
    std::string symbol;
    double shares = 0;
    double cost = 0;
    std::string entry_date;
};

struct ArenaTradeRecord {
    std::string bot, symbol, date;
    bool buy = true;
    double shares = 0, price = 0, value = 0, profit = 0;
    bool is_public = false;  // announced in Discord
};

// One politician stock trade, stored exactly as disclosed (see core/politicians/senate_efd.h
// for where this comes from and its limits).
struct PoliticianTradeRecord {
    int64_t id = 0;
    std::string chamber;  // "Senate" today; "House" once PDF parsing exists
    std::string first_name, last_name, office;
    std::string report_id;
    int item_number = 0;
    std::string filed_date, transaction_date;
    std::string owner, ticker, asset_name, asset_type, transaction_type;
    double amount_min = 0;
    std::optional<double> amount_max;
    std::string comment;
    std::string fetched_at;
    bool is_public = false;

    std::string full_name() const { return first_name + " " + last_name; }
};

// A user's "tell me when the price gets there" alert. direction: +1 = when the price rises to
// target or above, -1 = when it falls to target or below (set from the price when it was created).
// It fires once; triggered_at is then set and the alert is no longer checked.
struct PriceAlertRecord {
    int64_t id = 0;
    std::string user_id;
    std::string symbol;
    double target = 0;
    int direction = 1;
    std::string created_at;
    std::string triggered_at;
    double triggered_price = 0;
};

// Safety alert state for one holding (see core/safety.h). armed: the price has been comfortably
// above the warning line since the last warning, so the next fall into the zone warns again.
struct SafetyStateRecord {
    std::string user_id;
    std::string symbol;
    bool armed = false;
    std::string warned_at;
};

// One stock checked by the screener. *_edge = conservative lower bound of the hit rate
// (Wilson, 90%) minus the random-day baseline; above 0 means "beats chance even when skeptical".
struct ScreenResult {
    std::string symbol;
    std::string name;
    std::string currency = "USD";
    std::string category;
    double price = 0;
    int peak_signals = 0;
    int peak_correct = 0;
    double baseline_peak = 0;
    int support_signals = 0;
    int support_correct = 0;
    double baseline_support = 0;
    double peak_edge = 0;
    double support_edge = 0;
    int direction = 0;  // current swing: +1 rising (watch for PEAK), -1 falling (watch for SUPPORT)
    double peak_score = 0;
    double peak_cutoff = 0;
    double support_score = 0;
    double support_cutoff = 0;
    std::string screened_at;

    double best_edge() const { return peak_edge > support_edge ? peak_edge : support_edge; }
};

}  // namespace strojanka
