#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "core/database.h"
#include "core/models.h"

namespace strojanka {

// All SQL lives here. Every method is thread-safe (it locks the database mutex).
class Repository {
public:
    explicit Repository(Database& db) : db_(db) {}

    // users
    void ensure_user(const std::string& user_id, const std::string& channel_id);
    std::string user_notify_channel(const std::string& user_id);

    // watchlist
    bool add_watch(const std::string& user_id, const std::string& symbol);    // false if already watched
    bool remove_watch(const std::string& user_id, const std::string& symbol); // false if not watched
    bool is_watching(const std::string& user_id, const std::string& symbol);
    std::vector<std::string> user_watchlist(const std::string& user_id);
    std::vector<std::string> watched_symbols();
    std::vector<std::string> watchers(const std::string& symbol);

    // stocks
    void upsert_stock(const StockRecord& stock);
    std::optional<StockRecord> get_stock(const std::string& symbol);

    // price history
    void upsert_bars(const std::string& symbol, const std::vector<Bar>& bars);
    std::vector<Bar> load_bars(const std::string& symbol, int max_bars);  // oldest first

    // transactions
    int64_t add_transaction(const TransactionRecord& tx);
    std::vector<TransactionRecord> transactions(const std::string& user_id, const std::string& symbol = "");
    std::vector<std::string> traded_symbols(const std::string& user_id);

    // learned parameters
    std::optional<StockParamsRecord> get_params(const std::string& symbol);
    void save_params(const StockParamsRecord& params);

    // signals
    // Inserts unless a signal for the same (symbol, side, leg_key) exists; returns the new id or 0.
    int64_t insert_signal_if_new(const SignalRecord& signal);
    std::vector<SignalRecord> pending_signals(const std::string& symbol);
    std::vector<SignalRecord> recent_resolved_signals(const std::string& symbol, SignalSide side, int limit);
    std::optional<SignalRecord> latest_signal(const std::string& symbol);
    bool has_signal_since(const std::string& symbol, SignalSide side, const std::string& since_iso);
    void resolve_signal(int64_t id, SignalOutcome outcome);

    // notifications
    int64_t add_notification(int64_t signal_id, const std::string& user_id, const std::string& message);
    void mark_notification(int64_t id, bool delivered, const std::string& error);

    // news
    int add_news(const std::vector<NewsItem>& items);  // returns how many were new
    std::vector<NewsItem> recent_news(const std::string& symbol, const std::string& since_iso, int limit);  // newest first
    std::vector<SignalRecord> resolved_signals_with_news();

    // admins
    bool add_admin(const std::string& user_id, const std::string& added_by);  // false if already an admin
    bool remove_admin(const std::string& user_id);
    std::vector<std::string> admins();

    // statistics for the admin panel
    int count(const std::string& table);  // only known tables; 0 otherwise
    std::vector<std::string> user_ids();  // oldest first

    // arena
    std::vector<ArenaBotRecord> arena_bots();
    std::vector<ArenaPositionRecord> arena_positions(const std::string& bot);
    std::vector<std::pair<std::string, bool>> arena_orders(const std::string& bot);  // symbol, buy
    // Saves a bot's whole state after a trading session in one transaction.
    void save_arena_state(const ArenaBotRecord& bot, const std::vector<ArenaPositionRecord>& positions,
                          const std::vector<std::pair<std::string, bool>>& orders, const std::vector<ArenaTradeRecord>& new_trades,
                          const std::vector<std::pair<std::string, double>>& new_equity);
    void save_arena_bot(const ArenaBotRecord& bot);
    std::vector<ArenaTradeRecord> arena_trades(const std::string& bot, int limit);  // newest first
    std::vector<std::pair<std::string, double>> arena_equity(const std::string& bot, const std::string& since);  // oldest first
    void reset_arena_portfolio(const std::string& bot);  // positions, orders and equity history removed

    // trained AI models: {model_json, report_json, trained_at}
    void save_model(const std::string& name, const std::string& model_json, const std::string& report_json);
    std::optional<std::array<std::string, 3>> load_model(const std::string& name);

    // politician trades
    // Inserts new trades (ignoring ones already stored, by report_id+item_number) and returns
    // only the ones that were actually new, with their assigned id -- so a caller can announce
    // or notify about exactly those, never ones already seen on an earlier run.
    std::vector<PoliticianTradeRecord> add_politician_trades(const std::vector<PoliticianTradeRecord>& trades);
    bool has_politician_filing(const std::string& report_id);  // true if any trade from it is stored
    void mark_politician_trade_public(int64_t id);
    // name_filter: "" for everyone, otherwise a case-insensitive substring of "First Last".
    std::vector<PoliticianTradeRecord> recent_politician_trades(const std::string& name_filter, int limit);
    std::vector<std::string> known_politician_names();  // distinct "First Last", most active first

    bool add_politician_watch(const std::string& user_id, const std::string& politician);     // false if already watched
    bool remove_politician_watch(const std::string& user_id, const std::string& politician);   // false if not watched
    std::vector<std::string> user_politician_watchlist(const std::string& user_id);
    std::vector<std::string> politician_watchers(const std::string& politician);  // normalized name match

    // screener
    // Replaces all screener results with `results` in one transaction.
    void replace_screen_results(const std::vector<ScreenResult>& results);
    std::vector<ScreenResult> screen_results();  // best first
    std::string meta(const std::string& key);
    void set_meta(const std::string& key, const std::string& value);

    Database& database() { return db_; }

private:
    Database& db_;
};

}  // namespace strojanka
