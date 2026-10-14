#include "core/repository.h"

#include <algorithm>

#include "common/strings.h"

namespace strojanka {

using Lock = std::lock_guard<std::recursive_mutex>;

const char* to_string(SignalSide side) { return side == SignalSide::Peak ? "PEAK" : "SUPPORT"; }

const char* to_string(SignalOutcome outcome) {
    switch (outcome) {
        case SignalOutcome::Pending: return "PENDING";
        case SignalOutcome::Correct: return "CORRECT";
        case SignalOutcome::Wrong: return "WRONG";
        case SignalOutcome::Expired: return "EXPIRED";
    }
    return "PENDING";
}

SignalSide signal_side_from_string(const std::string& s) { return s == "SUPPORT" ? SignalSide::Support : SignalSide::Peak; }

SignalOutcome signal_outcome_from_string(const std::string& s) {
    if (s == "CORRECT") return SignalOutcome::Correct;
    if (s == "WRONG") return SignalOutcome::Wrong;
    if (s == "EXPIRED") return SignalOutcome::Expired;
    return SignalOutcome::Pending;
}

void Repository::ensure_user(const std::string& user_id, const std::string& channel_id) {
    Lock lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT INTO users(discord_id, notify_channel_id, created_at) VALUES (?1, ?2, ?3) "
        "ON CONFLICT(discord_id) DO UPDATE SET notify_channel_id = COALESCE(NULLIF(?2, ''), notify_channel_id);");
    st.bind(1, user_id).bind(2, channel_id).bind(3, utc_now_iso());
    st.step();
}

std::string Repository::user_notify_channel(const std::string& user_id) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT notify_channel_id FROM users WHERE discord_id = ?1;");
    st.bind(1, user_id);
    return st.step() ? st.text(0) : std::string();
}

bool Repository::add_watch(const std::string& user_id, const std::string& symbol) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("INSERT OR IGNORE INTO watchlist(discord_id, symbol, added_at) VALUES (?1, ?2, ?3);");
    st.bind(1, user_id).bind(2, symbol).bind(3, utc_now_iso());
    st.step();
    return db_.changes() > 0;
}

bool Repository::remove_watch(const std::string& user_id, const std::string& symbol) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("DELETE FROM watchlist WHERE discord_id = ?1 AND symbol = ?2;");
    st.bind(1, user_id).bind(2, symbol);
    st.step();
    return db_.changes() > 0;
}

bool Repository::is_watching(const std::string& user_id, const std::string& symbol) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT 1 FROM watchlist WHERE discord_id = ?1 AND symbol = ?2;");
    st.bind(1, user_id).bind(2, symbol);
    return st.step();
}

static std::vector<std::string> collect_strings(Statement& st) {
    std::vector<std::string> out;
    while (st.step()) out.push_back(st.text(0));
    return out;
}

std::vector<std::string> Repository::user_watchlist(const std::string& user_id) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT symbol FROM watchlist WHERE discord_id = ?1 ORDER BY symbol;");
    st.bind(1, user_id);
    return collect_strings(st);
}

std::vector<std::string> Repository::watched_symbols() {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT DISTINCT symbol FROM watchlist ORDER BY symbol;");
    return collect_strings(st);
}

std::vector<std::string> Repository::watchers(const std::string& symbol) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT discord_id FROM watchlist WHERE symbol = ?1;");
    st.bind(1, symbol);
    return collect_strings(st);
}

void Repository::upsert_stock(const StockRecord& s) {
    Lock lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT INTO stocks(symbol, name, currency, exchange, category, trend, last_price, last_price_at, "
        "support_level, resistance_level, peak_score, support_score, profile_json, history_full_refresh_at, "
        "created_at, updated_at, news_fetched_at) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?15,?16) "
        "ON CONFLICT(symbol) DO UPDATE SET name=?2, currency=?3, exchange=?4, category=?5, trend=?6, last_price=?7, "
        "last_price_at=?8, support_level=?9, resistance_level=?10, peak_score=?11, support_score=?12, "
        "profile_json=?13, history_full_refresh_at=?14, updated_at=?15, news_fetched_at=?16;");
    st.bind(1, s.symbol).bind(2, s.name).bind(3, s.currency).bind(4, s.exchange).bind(5, s.category);
    st.bind(6, s.trend).bind(7, s.last_price).bind(8, s.last_price_at).bind(9, s.support_level);
    st.bind(10, s.resistance_level).bind(11, s.peak_score).bind(12, s.support_score).bind(13, s.profile_json);
    st.bind(14, s.history_full_refresh_at).bind(15, utc_now_iso()).bind(16, s.news_fetched_at);
    st.step();
}

std::optional<StockRecord> Repository::get_stock(const std::string& symbol) {
    Lock lock(db_.mutex());
    auto st = db_.prepare(
        "SELECT symbol, name, currency, exchange, category, trend, last_price, last_price_at, support_level, "
        "resistance_level, peak_score, support_score, profile_json, history_full_refresh_at, news_fetched_at FROM stocks "
        "WHERE symbol = ?1;");
    st.bind(1, symbol);
    if (!st.step()) return std::nullopt;
    StockRecord s;
    s.symbol = st.text(0);
    s.name = st.text(1);
    s.currency = st.text(2);
    s.exchange = st.text(3);
    s.category = st.text(4);
    s.trend = st.text(5);
    s.last_price = st.real(6);
    s.last_price_at = st.text(7);
    s.support_level = st.real(8);
    s.resistance_level = st.real(9);
    s.peak_score = st.real(10);
    s.support_score = st.real(11);
    s.profile_json = st.text(12);
    s.history_full_refresh_at = st.text(13);
    s.news_fetched_at = st.text(14);
    return s;
}

void Repository::upsert_bars(const std::string& symbol, const std::vector<Bar>& bars) {
    if (bars.empty()) return;
    SqlTransaction tx(db_);
    auto st = db_.prepare(
        "INSERT INTO price_history(symbol, bar_date, open, high, low, close, volume) VALUES (?1,?2,?3,?4,?5,?6,?7) "
        "ON CONFLICT(symbol, bar_date) DO UPDATE SET open=?3, high=?4, low=?5, close=?6, volume=?7;");
    for (const auto& b : bars) {
        st.reset();
        st.bind(1, symbol).bind(2, b.date).bind(3, b.open).bind(4, b.high).bind(5, b.low).bind(6, b.close).bind(7, b.volume);
        st.step();
    }
    tx.commit();
}

std::vector<Bar> Repository::load_bars(const std::string& symbol, int max_bars) {
    Lock lock(db_.mutex());
    auto st = db_.prepare(
        "SELECT bar_date, open, high, low, close, volume FROM price_history WHERE symbol = ?1 "
        "ORDER BY bar_date DESC LIMIT ?2;");
    st.bind(1, symbol).bind(2, max_bars);
    std::vector<Bar> bars;
    while (st.step()) {
        Bar b;
        b.date = st.text(0);
        b.open = st.real(1);
        b.high = st.real(2);
        b.low = st.real(3);
        b.close = st.real(4);
        b.volume = st.real(5);
        bars.push_back(b);
    }
    std::reverse(bars.begin(), bars.end());
    return bars;
}

int64_t Repository::add_transaction(const TransactionRecord& t) {
    Lock lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT INTO transactions(discord_id, symbol, side, shares, total_amount, created_at) VALUES (?1,?2,?3,?4,?5,?6);");
    st.bind(1, t.user_id).bind(2, t.symbol).bind(3, t.side == TxSide::Buy ? "BUY" : "SELL");
    st.bind(4, t.shares).bind(5, t.total).bind(6, t.created_at.empty() ? utc_now_iso() : t.created_at);
    st.step();
    return db_.last_insert_id();
}

std::vector<TransactionRecord> Repository::transactions(const std::string& user_id, const std::string& symbol) {
    Lock lock(db_.mutex());
    auto st = db_.prepare(
        "SELECT id, discord_id, symbol, side, shares, total_amount, created_at FROM transactions "
        "WHERE discord_id = ?1 AND (?2 = '' OR symbol = ?2) ORDER BY created_at, id;");
    st.bind(1, user_id).bind(2, symbol);
    std::vector<TransactionRecord> out;
    while (st.step()) {
        TransactionRecord t;
        t.id = st.integer(0);
        t.user_id = st.text(1);
        t.symbol = st.text(2);
        t.side = st.text(3) == "SELL" ? TxSide::Sell : TxSide::Buy;
        t.shares = st.real(4);
        t.total = st.real(5);
        t.created_at = st.text(6);
        out.push_back(t);
    }
    return out;
}

std::vector<std::string> Repository::traded_symbols(const std::string& user_id) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT DISTINCT symbol FROM transactions WHERE discord_id = ?1 ORDER BY symbol;");
    st.bind(1, user_id);
    return collect_strings(st);
}

std::optional<StockParamsRecord> Repository::get_params(const std::string& symbol) {
    Lock lock(db_.mutex());
    auto st = db_.prepare(
        "SELECT symbol, params_json, calibration_score, calibrated_at, calibration_count, backtest_peak_signals, "
        "backtest_peak_correct, backtest_support_signals, backtest_support_correct, baseline_peak, baseline_support, "
        "backtest_at FROM stock_params WHERE symbol = ?1;");
    st.bind(1, symbol);
    if (!st.step()) return std::nullopt;
    StockParamsRecord p;
    p.symbol = st.text(0);
    p.params_json = st.text(1);
    p.calibration_score = st.real(2);
    p.calibrated_at = st.text(3);
    p.calibration_count = static_cast<int>(st.integer(4));
    p.backtest_peak_signals = static_cast<int>(st.integer(5));
    p.backtest_peak_correct = static_cast<int>(st.integer(6));
    p.backtest_support_signals = static_cast<int>(st.integer(7));
    p.backtest_support_correct = static_cast<int>(st.integer(8));
    p.baseline_peak = st.real(9);
    p.baseline_support = st.real(10);
    p.backtest_at = st.text(11);
    return p;
}

void Repository::save_params(const StockParamsRecord& p) {
    Lock lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT INTO stock_params(symbol, params_json, calibration_score, calibrated_at, calibration_count, "
        "backtest_peak_signals, backtest_peak_correct, backtest_support_signals, backtest_support_correct, "
        "baseline_peak, baseline_support, backtest_at) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12) "
        "ON CONFLICT(symbol) DO UPDATE SET params_json=?2, calibration_score=?3, calibrated_at=?4, "
        "calibration_count=?5, backtest_peak_signals=?6, backtest_peak_correct=?7, backtest_support_signals=?8, "
        "backtest_support_correct=?9, baseline_peak=?10, baseline_support=?11, backtest_at=?12;");
    st.bind(1, p.symbol).bind(2, p.params_json).bind(3, p.calibration_score).bind(4, p.calibrated_at);
    st.bind(5, p.calibration_count).bind(6, p.backtest_peak_signals).bind(7, p.backtest_peak_correct);
    st.bind(8, p.backtest_support_signals).bind(9, p.backtest_support_correct).bind(10, p.baseline_peak);
    st.bind(11, p.baseline_support).bind(12, p.backtest_at);
    st.step();
}

int64_t Repository::insert_signal_if_new(const SignalRecord& s) {
    Lock lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT OR IGNORE INTO signals(symbol, side, bar_date, price, score, threshold, leg_key, move_pct, "
        "horizon_bars, params_json, outcome, created_at, news_sentiment) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,'PENDING',?11,?12);");
    st.bind(1, s.symbol).bind(2, to_string(s.side)).bind(3, s.bar_date).bind(4, s.price).bind(5, s.score);
    st.bind(6, s.threshold).bind(7, s.leg_key).bind(8, s.move_pct).bind(9, s.horizon_bars);
    st.bind(10, s.params_json).bind(11, utc_now_iso());
    if (s.news_sentiment) st.bind(12, *s.news_sentiment);
    else st.bind_null(12);
    st.step();
    return db_.changes() > 0 ? db_.last_insert_id() : 0;
}

static SignalRecord read_signal(Statement& st) {
    SignalRecord s;
    s.id = st.integer(0);
    s.symbol = st.text(1);
    s.side = signal_side_from_string(st.text(2));
    s.bar_date = st.text(3);
    s.price = st.real(4);
    s.score = st.real(5);
    s.threshold = st.real(6);
    s.leg_key = st.text(7);
    s.move_pct = st.real(8);
    s.horizon_bars = static_cast<int>(st.integer(9));
    s.params_json = st.text(10);
    s.outcome = signal_outcome_from_string(st.text(11));
    s.created_at = st.text(12);
    s.resolved_at = st.text(13);
    if (!st.is_null(14)) s.news_sentiment = st.real(14);
    return s;
}

static const char* kSignalColumns =
    "id, symbol, side, bar_date, price, score, threshold, leg_key, move_pct, horizon_bars, params_json, outcome, "
    "created_at, resolved_at, news_sentiment";

std::vector<SignalRecord> Repository::pending_signals(const std::string& symbol) {
    Lock lock(db_.mutex());
    auto st = db_.prepare(std::string("SELECT ") + kSignalColumns +
                          " FROM signals WHERE symbol = ?1 AND outcome = 'PENDING' ORDER BY id;");
    st.bind(1, symbol);
    std::vector<SignalRecord> out;
    while (st.step()) out.push_back(read_signal(st));
    return out;
}

std::vector<SignalRecord> Repository::recent_resolved_signals(const std::string& symbol, SignalSide side, int limit) {
    Lock lock(db_.mutex());
    auto st = db_.prepare(std::string("SELECT ") + kSignalColumns +
                          " FROM signals WHERE symbol = ?1 AND side = ?2 AND outcome != 'PENDING' "
                          "ORDER BY id DESC LIMIT ?3;");
    st.bind(1, symbol).bind(2, to_string(side)).bind(3, limit);
    std::vector<SignalRecord> out;
    while (st.step()) out.push_back(read_signal(st));
    return out;
}

std::optional<SignalRecord> Repository::latest_signal(const std::string& symbol) {
    Lock lock(db_.mutex());
    auto st = db_.prepare(std::string("SELECT ") + kSignalColumns + " FROM signals WHERE symbol = ?1 ORDER BY id DESC LIMIT 1;");
    st.bind(1, symbol);
    if (!st.step()) return std::nullopt;
    return read_signal(st);
}

bool Repository::has_signal_since(const std::string& symbol, SignalSide side, const std::string& since_iso) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT 1 FROM signals WHERE symbol = ?1 AND side = ?2 AND created_at >= ?3 LIMIT 1;");
    st.bind(1, symbol).bind(2, to_string(side)).bind(3, since_iso);
    return st.step();
}

void Repository::resolve_signal(int64_t id, SignalOutcome outcome) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("UPDATE signals SET outcome = ?2, resolved_at = ?3 WHERE id = ?1;");
    st.bind(1, id).bind(2, to_string(outcome)).bind(3, utc_now_iso());
    st.step();
}

int64_t Repository::add_notification(int64_t signal_id, const std::string& user_id, const std::string& message) {
    Lock lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT INTO notifications(signal_id, discord_id, message, created_at) VALUES (?1, ?2, ?3, ?4);");
    st.bind(1, signal_id).bind(2, user_id).bind(3, message).bind(4, utc_now_iso());
    st.step();
    return db_.last_insert_id();
}

void Repository::mark_notification(int64_t id, bool delivered, const std::string& error) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("UPDATE notifications SET delivered = ?2, error = NULLIF(?3, '') WHERE id = ?1;");
    st.bind(1, id).bind(2, delivered ? 1 : 0).bind(3, error);
    st.step();
}

int Repository::add_news(const std::vector<NewsItem>& items) {
    if (items.empty()) return 0;
    SqlTransaction tx(db_);
    auto st = db_.prepare(
        "INSERT OR IGNORE INTO news_items(symbol, title, link, source, published_at, sentiment, red_flag, title_key, "
        "fetched_at) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9);");
    int added = 0;
    const std::string now = utc_now_iso();
    for (const auto& n : items) {
        st.reset();
        st.bind(1, n.symbol).bind(2, n.title).bind(3, n.link).bind(4, n.source).bind(5, n.published_at);
        st.bind(6, n.sentiment).bind(7, n.red_flag ? 1 : 0).bind(8, n.title_key).bind(9, now);
        st.step();
        added += db_.changes();
    }
    tx.commit();
    return added;
}

std::vector<NewsItem> Repository::recent_news(const std::string& symbol, const std::string& since_iso, int limit) {
    Lock lock(db_.mutex());
    auto st = db_.prepare(
        "SELECT symbol, title, link, source, published_at, sentiment, red_flag, title_key FROM news_items "
        "WHERE symbol = ?1 AND published_at >= ?2 ORDER BY published_at DESC LIMIT ?3;");
    st.bind(1, symbol).bind(2, since_iso).bind(3, limit);
    std::vector<NewsItem> out;
    while (st.step()) {
        NewsItem n;
        n.symbol = st.text(0);
        n.title = st.text(1);
        n.link = st.text(2);
        n.source = st.text(3);
        n.published_at = st.text(4);
        n.sentiment = st.real(5);
        n.red_flag = st.integer(6) != 0;
        n.title_key = st.text(7);
        out.push_back(n);
    }
    return out;
}

std::vector<SignalRecord> Repository::resolved_signals_with_news() {
    Lock lock(db_.mutex());
    auto st = db_.prepare(std::string("SELECT ") + kSignalColumns +
                          " FROM signals WHERE outcome != 'PENDING' AND news_sentiment IS NOT NULL ORDER BY id;");
    std::vector<SignalRecord> out;
    while (st.step()) out.push_back(read_signal(st));
    return out;
}

bool Repository::add_admin(const std::string& user_id, const std::string& added_by) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("INSERT OR IGNORE INTO admins(discord_id, added_by, added_at) VALUES (?1, ?2, ?3);");
    st.bind(1, user_id).bind(2, added_by).bind(3, utc_now_iso());
    st.step();
    return db_.changes() > 0;
}

bool Repository::remove_admin(const std::string& user_id) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("DELETE FROM admins WHERE discord_id = ?1;");
    st.bind(1, user_id);
    st.step();
    return db_.changes() > 0;
}

std::vector<std::string> Repository::admins() {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT discord_id FROM admins ORDER BY added_at;");
    return collect_strings(st);
}

int Repository::count(const std::string& table) {
    static const std::vector<std::string> allowed = {"users", "watchlist", "transactions", "signals", "news_items",
                                                     "price_history", "stocks", "screen_results"};
    if (std::find(allowed.begin(), allowed.end(), table) == allowed.end()) return 0;
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT COUNT(*) FROM " + table + ";");
    return st.step() ? static_cast<int>(st.integer(0)) : 0;
}

std::vector<std::string> Repository::user_ids() {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT discord_id FROM users ORDER BY created_at;");
    return collect_strings(st);
}

std::vector<ArenaBotRecord> Repository::arena_bots() {
    Lock lock(db_.mutex());
    auto st = db_.prepare(
        "SELECT name, stage, cash, peak_value, last_date, live_started, stage_changed, backtest_json, backtested_at "
        "FROM arena_bots;");
    std::vector<ArenaBotRecord> out;
    while (st.step()) {
        ArenaBotRecord b;
        b.name = st.text(0);
        b.stage = st.text(1);
        b.cash = st.real(2);
        b.peak_value = st.real(3);
        b.last_date = st.text(4);
        b.live_started = st.text(5);
        b.stage_changed = st.text(6);
        b.backtest_json = st.text(7);
        b.backtested_at = st.text(8);
        out.push_back(b);
    }
    return out;
}

std::vector<ArenaPositionRecord> Repository::arena_positions(const std::string& bot) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT symbol, shares, cost, entry_date FROM arena_positions WHERE bot = ?1 ORDER BY symbol;");
    st.bind(1, bot);
    std::vector<ArenaPositionRecord> out;
    while (st.step()) out.push_back({st.text(0), st.real(1), st.real(2), st.text(3)});
    return out;
}

std::vector<std::pair<std::string, bool>> Repository::arena_orders(const std::string& bot) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT symbol, buy FROM arena_orders WHERE bot = ?1;");
    st.bind(1, bot);
    std::vector<std::pair<std::string, bool>> out;
    while (st.step()) out.emplace_back(st.text(0), st.integer(1) != 0);
    return out;
}

void Repository::save_arena_bot(const ArenaBotRecord& b) {
    Lock lock(db_.mutex());
    auto st = db_.prepare(
        "INSERT INTO arena_bots(name, stage, cash, peak_value, last_date, live_started, stage_changed, backtest_json, "
        "backtested_at) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9) ON CONFLICT(name) DO UPDATE SET stage=?2, cash=?3, "
        "peak_value=?4, last_date=?5, live_started=?6, stage_changed=?7, backtest_json=?8, backtested_at=?9;");
    st.bind(1, b.name).bind(2, b.stage).bind(3, b.cash).bind(4, b.peak_value).bind(5, b.last_date);
    st.bind(6, b.live_started).bind(7, b.stage_changed).bind(8, b.backtest_json).bind(9, b.backtested_at);
    st.step();
}

void Repository::save_arena_state(const ArenaBotRecord& bot, const std::vector<ArenaPositionRecord>& positions,
                                  const std::vector<std::pair<std::string, bool>>& orders,
                                  const std::vector<ArenaTradeRecord>& new_trades,
                                  const std::vector<std::pair<std::string, double>>& new_equity) {
    SqlTransaction tx(db_);
    save_arena_bot(bot);
    {
        auto del = db_.prepare("DELETE FROM arena_positions WHERE bot = ?1;");
        del.bind(1, bot.name);
        del.step();
        auto del_orders = db_.prepare("DELETE FROM arena_orders WHERE bot = ?1;");
        del_orders.bind(1, bot.name);
        del_orders.step();
    }
    auto pos = db_.prepare("INSERT INTO arena_positions(bot, symbol, shares, cost, entry_date) VALUES (?1,?2,?3,?4,?5);");
    for (const auto& p : positions) {
        pos.reset();
        pos.bind(1, bot.name).bind(2, p.symbol).bind(3, p.shares).bind(4, p.cost).bind(5, p.entry_date);
        pos.step();
    }
    auto ord = db_.prepare("INSERT OR REPLACE INTO arena_orders(bot, symbol, buy) VALUES (?1,?2,?3);");
    for (const auto& [symbol, buy] : orders) {
        ord.reset();
        ord.bind(1, bot.name).bind(2, symbol).bind(3, buy ? 1 : 0);
        ord.step();
    }
    auto tr = db_.prepare(
        "INSERT INTO arena_trades(bot, symbol, buy, shares, price, value, profit, trade_date, public) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9);");
    for (const auto& t : new_trades) {
        tr.reset();
        tr.bind(1, t.bot).bind(2, t.symbol).bind(3, t.buy ? 1 : 0).bind(4, t.shares).bind(5, t.price);
        tr.bind(6, t.value).bind(7, t.profit).bind(8, t.date).bind(9, t.is_public ? 1 : 0);
        tr.step();
    }
    auto eq = db_.prepare("INSERT OR REPLACE INTO arena_equity(bot, day, value) VALUES (?1,?2,?3);");
    for (const auto& [day, value] : new_equity) {
        eq.reset();
        eq.bind(1, bot.name).bind(2, day).bind(3, value);
        eq.step();
    }
    tx.commit();
}

std::vector<ArenaTradeRecord> Repository::arena_trades(const std::string& bot, int limit) {
    Lock lock(db_.mutex());
    auto st = db_.prepare(
        "SELECT bot, symbol, buy, shares, price, value, profit, trade_date, public FROM arena_trades WHERE bot = ?1 "
        "ORDER BY id DESC LIMIT ?2;");
    st.bind(1, bot).bind(2, limit);
    std::vector<ArenaTradeRecord> out;
    while (st.step()) {
        ArenaTradeRecord t;
        t.bot = st.text(0);
        t.symbol = st.text(1);
        t.buy = st.integer(2) != 0;
        t.shares = st.real(3);
        t.price = st.real(4);
        t.value = st.real(5);
        t.profit = st.real(6);
        t.date = st.text(7);
        t.is_public = st.integer(8) != 0;
        out.push_back(t);
    }
    return out;
}

std::vector<std::pair<std::string, double>> Repository::arena_equity(const std::string& bot, const std::string& since) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT day, value FROM arena_equity WHERE bot = ?1 AND day >= ?2 ORDER BY day;");
    st.bind(1, bot).bind(2, since);
    std::vector<std::pair<std::string, double>> out;
    while (st.step()) out.emplace_back(st.text(0), st.real(1));
    return out;
}

void Repository::reset_arena_portfolio(const std::string& bot) {
    SqlTransaction tx(db_);
    for (const char* table : {"arena_positions", "arena_orders", "arena_equity"}) {
        auto st = db_.prepare(std::string("DELETE FROM ") + table + " WHERE bot = ?1;");
        st.bind(1, bot);
        st.step();
    }
    tx.commit();
}

void Repository::save_model(const std::string& name, const std::string& model_json, const std::string& report_json) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("INSERT OR REPLACE INTO ml_models(name, model_json, report_json, trained_at) VALUES (?1,?2,?3,?4);");
    st.bind(1, name).bind(2, model_json).bind(3, report_json).bind(4, utc_now_iso());
    st.step();
}

std::optional<std::array<std::string, 3>> Repository::load_model(const std::string& name) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT model_json, report_json, trained_at FROM ml_models WHERE name = ?1;");
    st.bind(1, name);
    if (!st.step()) return std::nullopt;
    return std::array<std::string, 3>{st.text(0), st.text(1), st.text(2)};
}

std::vector<PoliticianTradeRecord> Repository::add_politician_trades(const std::vector<PoliticianTradeRecord>& trades) {
    std::vector<PoliticianTradeRecord> added;
    SqlTransaction tx(db_);
    auto st = db_.prepare(
        "INSERT OR IGNORE INTO politician_trades(chamber, first_name, last_name, office, report_id, item_number, "
        "filed_date, transaction_date, owner, ticker, asset_name, asset_type, transaction_type, amount_min, "
        "amount_max, has_amount_max, comment, fetched_at) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18);");
    for (auto t : trades) {
        st.reset();
        st.bind(1, t.chamber).bind(2, t.first_name).bind(3, t.last_name).bind(4, t.office);
        st.bind(5, t.report_id).bind(6, t.item_number).bind(7, t.filed_date).bind(8, t.transaction_date);
        st.bind(9, t.owner).bind(10, t.ticker).bind(11, t.asset_name).bind(12, t.asset_type).bind(13, t.transaction_type);
        st.bind(14, t.amount_min).bind(15, t.amount_max.value_or(0)).bind(16, t.amount_max.has_value() ? 1 : 0);
        st.bind(17, t.comment).bind(18, t.fetched_at);
        st.step();
        if (db_.changes() > 0) {
            t.id = db_.last_insert_id();
            added.push_back(t);
        }
    }
    tx.commit();
    return added;
}

bool Repository::has_politician_filing(const std::string& report_id) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT 1 FROM politician_trades WHERE report_id = ?1 LIMIT 1;");
    st.bind(1, report_id);
    return st.step();
}

void Repository::mark_politician_trade_public(int64_t id) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("UPDATE politician_trades SET public = 1 WHERE id = ?1;");
    st.bind(1, id);
    st.step();
}

namespace {
PoliticianTradeRecord read_politician_trade(Statement& st) {
    PoliticianTradeRecord t;
    t.id = st.integer(0);
    t.chamber = st.text(1);
    t.first_name = st.text(2);
    t.last_name = st.text(3);
    t.office = st.text(4);
    t.report_id = st.text(5);
    t.item_number = static_cast<int>(st.integer(6));
    t.filed_date = st.text(7);
    t.transaction_date = st.text(8);
    t.owner = st.text(9);
    t.ticker = st.text(10);
    t.asset_name = st.text(11);
    t.asset_type = st.text(12);
    t.transaction_type = st.text(13);
    t.amount_min = st.real(14);
    if (st.integer(16) != 0) t.amount_max = st.real(15);
    t.comment = st.text(17);
    t.fetched_at = st.text(18);
    t.is_public = st.integer(19) != 0;
    return t;
}
constexpr const char* kPoliticianTradeColumns =
    "id, chamber, first_name, last_name, office, report_id, item_number, filed_date, transaction_date, owner, "
    "ticker, asset_name, asset_type, transaction_type, amount_min, amount_max, has_amount_max, comment, fetched_at, public";
}  // namespace

std::vector<PoliticianTradeRecord> Repository::recent_politician_trades(const std::string& name_filter, int limit) {
    Lock lock(db_.mutex());
    std::string sql = std::string("SELECT ") + kPoliticianTradeColumns + " FROM politician_trades";
    if (!name_filter.empty()) sql += " WHERE lower(first_name || ' ' || last_name) LIKE ?1";
    sql += " ORDER BY id DESC LIMIT ?" + std::to_string(name_filter.empty() ? 1 : 2) + ";";
    auto st = db_.prepare(sql);
    if (!name_filter.empty()) st.bind(1, "%" + to_lower(name_filter) + "%").bind(2, limit);
    else st.bind(1, limit);
    std::vector<PoliticianTradeRecord> out;
    while (st.step()) out.push_back(read_politician_trade(st));
    return out;
}

std::vector<std::string> Repository::known_politician_names() {
    Lock lock(db_.mutex());
    auto st = db_.prepare(
        "SELECT first_name || ' ' || last_name AS name FROM politician_trades GROUP BY name ORDER BY COUNT(*) DESC;");
    return collect_strings(st);
}

bool Repository::add_politician_watch(const std::string& user_id, const std::string& politician) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("INSERT OR IGNORE INTO politician_watch(discord_id, politician, added_at) VALUES (?1,?2,?3);");
    st.bind(1, user_id).bind(2, politician).bind(3, utc_now_iso());
    st.step();
    return db_.changes() > 0;
}

bool Repository::remove_politician_watch(const std::string& user_id, const std::string& politician) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("DELETE FROM politician_watch WHERE discord_id = ?1 AND politician = ?2;");
    st.bind(1, user_id).bind(2, politician);
    st.step();
    return db_.changes() > 0;
}

std::vector<std::string> Repository::user_politician_watchlist(const std::string& user_id) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT politician FROM politician_watch WHERE discord_id = ?1 ORDER BY added_at;");
    st.bind(1, user_id);
    return collect_strings(st);
}

std::vector<std::string> Repository::politician_watchers(const std::string& politician) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT discord_id FROM politician_watch WHERE politician = ?1;");
    st.bind(1, politician);
    return collect_strings(st);
}

void Repository::replace_screen_results(const std::vector<ScreenResult>& results) {
    SqlTransaction tx(db_);
    db_.exec("DELETE FROM screen_results;");
    auto st = db_.prepare(
        "INSERT INTO screen_results(symbol, name, currency, category, price, peak_signals, peak_correct, baseline_peak, "
        "support_signals, support_correct, baseline_support, peak_edge, support_edge, direction, peak_score, "
        "peak_cutoff, support_score, support_cutoff, screened_at) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19);");
    for (const auto& r : results) {
        st.reset();
        st.bind(1, r.symbol).bind(2, r.name).bind(3, r.currency).bind(4, r.category).bind(5, r.price);
        st.bind(6, r.peak_signals).bind(7, r.peak_correct).bind(8, r.baseline_peak);
        st.bind(9, r.support_signals).bind(10, r.support_correct).bind(11, r.baseline_support);
        st.bind(12, r.peak_edge).bind(13, r.support_edge).bind(14, r.direction).bind(15, r.peak_score);
        st.bind(16, r.peak_cutoff).bind(17, r.support_score).bind(18, r.support_cutoff).bind(19, r.screened_at);
        st.step();
    }
    tx.commit();
}

std::vector<ScreenResult> Repository::screen_results() {
    Lock lock(db_.mutex());
    auto st = db_.prepare(
        "SELECT symbol, name, currency, category, price, peak_signals, peak_correct, baseline_peak, support_signals, "
        "support_correct, baseline_support, peak_edge, support_edge, direction, peak_score, peak_cutoff, "
        "support_score, support_cutoff, screened_at FROM screen_results "
        "ORDER BY MAX(peak_edge, support_edge) DESC;");
    std::vector<ScreenResult> out;
    while (st.step()) {
        ScreenResult r;
        r.symbol = st.text(0);
        r.name = st.text(1);
        r.currency = st.text(2);
        r.category = st.text(3);
        r.price = st.real(4);
        r.peak_signals = static_cast<int>(st.integer(5));
        r.peak_correct = static_cast<int>(st.integer(6));
        r.baseline_peak = st.real(7);
        r.support_signals = static_cast<int>(st.integer(8));
        r.support_correct = static_cast<int>(st.integer(9));
        r.baseline_support = st.real(10);
        r.peak_edge = st.real(11);
        r.support_edge = st.real(12);
        r.direction = static_cast<int>(st.integer(13));
        r.peak_score = st.real(14);
        r.peak_cutoff = st.real(15);
        r.support_score = st.real(16);
        r.support_cutoff = st.real(17);
        r.screened_at = st.text(18);
        out.push_back(r);
    }
    return out;
}

std::string Repository::meta(const std::string& key) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("SELECT value FROM app_meta WHERE key = ?1;");
    st.bind(1, key);
    return st.step() ? st.text(0) : std::string();
}

void Repository::set_meta(const std::string& key, const std::string& value) {
    Lock lock(db_.mutex());
    auto st = db_.prepare("INSERT OR REPLACE INTO app_meta(key, value) VALUES (?1, ?2);");
    st.bind(1, key).bind(2, value);
    st.step();
}

}  // namespace strojanka
