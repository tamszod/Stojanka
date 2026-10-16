#include "core/arena/arena_service.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <sstream>

#include <nlohmann/json.hpp>

#include "common/strings.h"
#include "core/admin_service.h"
#include "core/log.h"
#include "core/screener.h"
#include "core/thread_pool.h"

namespace strojanka::arena {

namespace {

constexpr int64_t kRefreshSeconds = 6 * 3600;
constexpr int64_t kBacktestEverySeconds = 7 * 86400;
constexpr size_t kFullHistoryBars = 1500;
constexpr int kLoadBars = 2600;
constexpr const char* kWeekKey = "arena_week";

std::string pad(const std::string& s, size_t width) {
    const size_t chars = utf8_length(s);
    return chars >= width ? s + " " : s + std::string(width - chars, ' ');
}

std::string signed_percent(double v) { return (v >= 0 ? "+" : "") + format_percent(v, 1); }

nlohmann::json backtest_json(const BacktestResult& r) {
    const Metrics& m = r.metrics;
    return {{"passed", r.passed}, {"verdict", r.verdict}, {"from", m.from}, {"to", m.to}, {"final_value", m.final_value},
            {"total_return", m.total_return}, {"cagr", m.cagr}, {"sharpe", m.sharpe}, {"max_drawdown", m.max_drawdown},
            {"trades", m.trades}, {"closed_trades", m.closed_trades}, {"win_rate", m.win_rate},
            {"thirds", {m.thirds[0], m.thirds[1], m.thirds[2]}}};
}

nlohmann::json parse(const std::string& text) {
    auto j = nlohmann::json::parse(text, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

// Index of the first calendar day after `date` (dates are ISO, so string order is time order).
int first_day_after(const Universe& u, const std::string& date) {
    return static_cast<int>(std::upper_bound(u.calendar.begin(), u.calendar.end(), date) - u.calendar.begin());
}

double drawdown_of(const std::vector<std::pair<std::string, double>>& rows) {
    double peak = 0, dd = 0;
    for (const auto& [day, v] : rows) {
        peak = std::max(peak, v);
        if (peak > 0) dd = std::max(dd, 1 - v / peak);
    }
    return dd;
}

std::string stage_label(const ArenaBotRecord& b, int days) {
    if (b.stage == "benchmark") return "benchmark";
    if (b.stage == "public") return "PUBLIC";
    if (b.stage == "proving") return "proving " + std::to_string(std::min(days, ArenaService::kProvingDays)) + "/" +
                                     std::to_string(ArenaService::kProvingDays) + " days";
    return "training";
}

}  // namespace

ArenaService::ArenaService(Repository& repo, MarketDataProvider& provider, const Config& config)
    : repo_(repo), provider_(provider), config_(config) {}

ArenaService::~ArenaService() { stop(); }

void ArenaService::set_announcer(std::function<void(const std::string&)> announcer) { announcer_ = std::move(announcer); }

void ArenaService::announce(const std::string& message) {
    if (announcer_) announcer_(message);
    else log_info("arena (no channel): " + message.substr(0, 120));
}

std::string ArenaService::channel_id() { return repo_.meta(kPublicChannelKey); }
void ArenaService::set_channel_id(const std::string& id) { repo_.set_meta(kPublicChannelKey, id); }

void ArenaService::start() {
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] {
        for (;;) {
            try {
                tick(last_complete_date(unix_now()));
            } catch (const std::exception& e) {
                log_error(std::string("arena failed: ") + e.what());
            } catch (...) {
                log_error("arena failed with an unknown error");
            }
            std::unique_lock<std::mutex> lock(loop_mutex_);
            if (loop_cv_.wait_for(lock, std::chrono::hours(1), [this] { return stopping_; })) break;
        }
    });
}

void ArenaService::stop() {
    {
        std::lock_guard<std::mutex> lock(loop_mutex_);
        stopping_ = true;
    }
    loop_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void ArenaService::refresh_prices(bool force_full) {
    const auto symbols = Screener::universe_for(config_);
    ThreadPool pool(static_cast<size_t>(std::max(1, config_.max_parallel_fetches)));
    std::vector<std::future<void>> jobs;
    const int64_t now = unix_now();
    for (const auto& symbol : symbols) {
        if (!force_full && fetched_at_.count(symbol) && now - fetched_at_[symbol] < kRefreshSeconds) continue;
        fetched_at_[symbol] = now;
        jobs.push_back(pool.submit([this, symbol, force_full] {
            const bool full = force_full || repo_.load_bars(symbol, static_cast<int>(kFullHistoryBars)).size() < kFullHistoryBars;
            std::string error;
            if (auto data = provider_.fetch(symbol, full ? HistoryRange::TenYears : HistoryRange::Month, &error)) {
                repo_.upsert_bars(symbol, data->bars);
            } else {
                log_warn("arena: " + symbol + ": " + error);
            }
        }));
    }
    for (auto& j : jobs) j.get();
}

bool ArenaService::rebuild_universe() {
    std::map<std::string, std::vector<Bar>> bars;
    for (const auto& symbol : Screener::universe_for(config_)) {
        auto b = repo_.load_bars(symbol, kLoadBars);
        if (!b.empty()) bars[symbol] = std::move(b);
    }
    Universe u = build_universe(bars);
    std::lock_guard<std::mutex> lock(view_mutex_);
    universe_ = std::move(u);
    return !universe_.calendar.empty();
}

void ArenaService::ensure_bots(const std::string& day) {
    const auto existing = repo_.arena_bots();
    for (const auto& spec : roster()) {
        const bool found = std::any_of(existing.begin(), existing.end(), [&](const ArenaBotRecord& b) { return b.name == spec.name; });
        if (found) continue;
        ArenaBotRecord b;
        b.name = spec.name;
        if (spec.kind == Kind::BuyHold) {
            b.stage = "benchmark";
            b.live_started = day;
            b.stage_changed = day;
        }
        repo_.save_arena_bot(b);
    }
}

bool ArenaService::backtests_due() {
    const auto bots = repo_.arena_bots();
    if (bots.size() < roster().size()) return true;
    return std::any_of(bots.begin(), bots.end(), [](const ArenaBotRecord& b) {
        return b.backtested_at.empty() || unix_now() - iso_to_unix(b.backtested_at) > kBacktestEverySeconds;
    });
}

void ArenaService::run_backtests(const std::string& day) {
    if (!backtests_due()) return;
    auto bots = repo_.arena_bots();
    log_info("arena: testing " + std::to_string(roster().size()) + " bots on " + std::to_string(universe_.calendar.size()) +
             " days of history");
    for (auto& bot : bots) {
        const Spec* spec = find_spec(bot.name);
        if (!spec) continue;
        const BacktestResult r = backtest(*spec, universe_);
        bot.backtest_json = backtest_json(r).dump();
        bot.backtested_at = utc_now_iso();
        log_info("arena: " + bot.name + " " + (r.passed ? "passed" : "did not pass") + " the history test (" + r.verdict + ")");
        if (bot.stage == "training" && r.passed) {
            repo_.reset_arena_portfolio(bot.name);
            bot.stage = "proving";
            bot.cash = kStartingCash;
            bot.peak_value = kStartingCash;
            bot.live_started = day;
            bot.stage_changed = day;
            bot.last_date.clear();
            log_info("arena: " + bot.name + " starts proving itself with live prices and $1,000");
        } else if ((bot.stage == "proving" || bot.stage == "public") && !r.passed) {
            if (bot.stage == "public") {
                announce("\xE2\x9A\xA0\xEF\xB8\x8F **" + bot.name + "** no longer passes the test on historical prices (" + r.verdict +
                         "), so it goes back into training and stops trading in public.");
            }
            bot.stage = "training";
            bot.stage_changed = day;
        }
        repo_.save_arena_bot(bot);
    }
}

void ArenaService::advance(ArenaBotRecord& bot, const Spec& spec, int to_day) {
    const Universe& u = universe_;
    int from = bot.last_date.empty() ? u.day_of(bot.live_started) : first_day_after(u, bot.last_date);
    if (bot.last_date.empty() && from < 0) from = first_day_after(u, bot.live_started) - 1;
    if (from < 0 || from > to_day) return;

    State state;
    state.cash = bot.cash;
    state.peak = bot.peak_value;
    for (const auto& p : repo_.arena_positions(bot.name)) {
        const int entry = first_day_after(u, p.entry_date) - 1;
        state.positions[p.symbol] = {p.shares, p.cost, std::max(0, entry)};
    }
    for (const auto& [symbol, buy] : repo_.arena_orders(bot.name)) state.orders.push_back({symbol, buy});

    std::vector<Fill> fills;
    std::vector<std::pair<std::string, double>> equity;
    for (int day = from; day <= to_day; ++day) equity.emplace_back(u.calendar[day], step(spec, u, day, state, fills));

    const bool is_public = bot.stage == "public";
    std::vector<ArenaTradeRecord> trades;
    for (const auto& f : fills) {
        trades.push_back({bot.name, f.symbol, u.calendar[f.day], f.buy, f.shares, f.price, f.value, f.profit, is_public});
    }
    std::vector<ArenaPositionRecord> positions;
    for (const auto& [symbol, p] : state.positions) positions.push_back({symbol, p.shares, p.cost, u.calendar[p.entry_day]});
    std::vector<std::pair<std::string, bool>> orders;
    for (const auto& o : state.orders) orders.emplace_back(o.symbol, o.buy);
    bot.cash = state.cash;
    bot.peak_value = state.peak;
    bot.last_date = u.calendar[to_day];
    repo_.save_arena_state(bot, positions, orders, trades, equity);

    if (is_public && !trades.empty()) {
        std::map<std::string, std::vector<const ArenaTradeRecord*>> by_day;
        for (const auto& t : trades) by_day[t.date].push_back(&t);
        for (const auto& [day, list] : by_day) {
            std::ostringstream m;
            m << "\xF0\x9F\x8F\x9B\xEF\xB8\x8F **" << bot.name << "** traded on " << day << ":\n";
            for (const auto* t : list) {
                m << "- " << (t->buy ? "bought" : "sold") << " **" << t->symbol << "**: " << format_double(t->shares, 4)
                  << " shares at " << format_money(t->price) << " (" << format_money(t->value) << ")";
                if (!t->buy) m << ", profit **" << format_signed_money(t->profit) << "**";
                m << "\n";
            }
            const double value = equity.back().second;
            m << "Portfolio: **" << format_money(value) << "** (" << signed_percent(value / kStartingCash - 1) << " since "
              << bot.live_started << ")";
            announce(m.str());
        }
    }
}

void ArenaService::judge(ArenaBotRecord& bot, const std::string& day) {
    auto atlas_return = [this](const std::string& from, const std::string& to) {
        const auto rows = repo_.arena_equity("Atlas", from);
        double first = 0, last = 0;
        for (const auto& [d, v] : rows) {
            if (d > to) break;
            if (first == 0) first = v;
            last = v;
        }
        return first > 0 ? last / first - 1 : 0.0;
    };

    if (bot.stage == "proving") {
        auto rows = repo_.arena_equity(bot.name, bot.stage_changed);
        if (static_cast<int>(rows.size()) <= kProvingDays) return;
        rows.erase(rows.begin(), rows.end() - (kProvingDays + 1));  // judge the most recent 40 trading days
        const double ret = rows.back().second / rows.front().second - 1;
        const double atlas = atlas_return(rows.front().first, rows.back().first);
        const double dd = drawdown_of(rows);
        if (ret >= atlas - kProvingTolerance && dd <= kProvingMaxDrawdown) {
            bot.stage = "public";
            bot.stage_changed = day;
            repo_.save_arena_bot(bot);
            const Spec* spec = find_spec(bot.name);
            announce("\xF0\x9F\x8E\x89 **" + bot.name + "** (" + (spec ? spec->origin : "") + ") has proven itself and now trades in public!\n" +
                     (spec ? spec->idea + "\n" : "") + "It passed the test on " +
                     "about 8 years of historical prices, then traded live for " + std::to_string(kProvingDays) +
                     " days: " + signed_percent(ret) + " vs " + signed_percent(atlas) + " for Atlas (the whole market), worst fall " +
                     format_percent(dd, 1) + ". Its portfolio is worth " + format_money(rows.back().second) + ".");
        }
        return;
    }
    if (bot.stage == "public") {
        auto rows = repo_.arena_equity(bot.name, bot.stage_changed);
        if (rows.empty()) return;
        std::string reason;
        const double dd = 1 - rows.back().second / std::max_element(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second < b.second; })->second;
        if (dd > kPublicMaxDrawdown) reason = "it fell " + format_percent(dd, 0) + " from its high";
        if (reason.empty() && static_cast<int>(rows.size()) > kPublicReviewDays) {
            rows.erase(rows.begin(), rows.end() - (kPublicReviewDays + 1));
            const double ret = rows.back().second / rows.front().second - 1;
            const double atlas = atlas_return(rows.front().first, rows.back().first);
            if (ret < atlas - kPublicMaxLag) {
                reason = "over the last " + std::to_string(kPublicReviewDays) + " days it made " + signed_percent(ret) +
                         " while the market made " + signed_percent(atlas);
            }
        }
        if (!reason.empty()) {
            bot.stage = "proving";
            bot.stage_changed = day;
            repo_.save_arena_bot(bot);
            announce("\xE2\x9A\xA0\xEF\xB8\x8F **" + bot.name + "** goes back to proving itself in private: " + reason +
                     ". It will trade in public again once it has done well for " + std::to_string(kProvingDays) + " days.");
        }
    }
}

std::string ArenaService::last_complete_date(int64_t unix_time) { return unix_to_date(unix_time - (21 * 3600 + 1800)); }

void ArenaService::tick(const std::string& last_complete, bool refresh) {
    std::lock_guard<std::mutex> lock(tick_mutex_);
    busy_ = true;
    struct Reset {
        std::atomic<bool>& flag;
        ~Reset() { flag = false; }
    } reset{busy_};

    if (refresh) refresh_prices(false);
    if (!rebuild_universe()) return;
    // Only finished trading days count: the newest bar may still change while the market is open.
    const int last = first_day_after(universe_, last_complete) - 1;
    if (last < kWarmupDays) return;
    const std::string day = universe_.calendar[last];

    // The AI model retrains together with the weekly tests, so the oracle bots' historical test
    // only ever uses predictions made by models that had not seen those years.
    if (pythia_) {
        if (backtests_due() || !pythia_->ready()) pythia_->train(universe_);
        pythia_->annotate(universe_);
    }
    ensure_bots(day);
    run_backtests(day);
    for (auto bot : repo_.arena_bots()) {
        const Spec* spec = find_spec(bot.name);
        if (!spec || (bot.stage != "benchmark" && bot.stage != "proving" && bot.stage != "public")) continue;
        advance(bot, *spec, last);
        judge(bot, day);
    }

    const auto bots = repo_.arena_bots();
    const bool any_public = std::any_of(bots.begin(), bots.end(), [](const ArenaBotRecord& b) { return b.stage == "public"; });
    const std::string week = std::to_string(iso_to_unix(day) / 86400 / 7);
    if (any_public && repo_.meta(kWeekKey) != week) {
        repo_.set_meta(kWeekKey, week);
        announce(leaderboard(false));
    }
}

BotView ArenaService::view_of(const ArenaBotRecord& b) {
    BotView v;
    v.name = b.name;
    v.stage = b.stage;
    if (const Spec* spec = find_spec(b.name)) {
        v.origin = spec->origin;
        v.idea = spec->idea;
    }
    const auto bt = parse(b.backtest_json);
    v.backtest_passed = bt.value("passed", false);
    v.verdict = bt.value("verdict", std::string(b.backtest_json.empty() ? "not tested yet" : ""));
    v.bt_return = bt.value("total_return", 0.0);
    v.bt_cagr = bt.value("cagr", 0.0);
    v.bt_sharpe = bt.value("sharpe", 0.0);
    v.bt_drawdown = bt.value("max_drawdown", 0.0);
    v.bt_trades = bt.value("closed_trades", 0);

    if (!b.live_started.empty() && b.stage != "training") {
        const auto rows = repo_.arena_equity(b.name, b.live_started);
        if (!rows.empty()) {
            v.value = rows.back().second;
            v.live_return = v.value / kStartingCash - 1;
            v.live_drawdown = drawdown_of(rows);
            const auto atlas = repo_.arena_equity("Atlas", rows.front().first);
            double first = 0, last = 0;
            for (const auto& [d, val] : atlas) {
                if (d > rows.back().first) break;
                if (first == 0) first = val;
                last = val;
            }
            v.vs_atlas = v.live_return - (first > 0 ? last / first - 1 : 0);
        }
        v.live_days = static_cast<int>(repo_.arena_equity(b.name, b.stage_changed).size());
        for (const auto& t : repo_.arena_trades(b.name, 100000)) v.live_trades += t.date >= b.live_started;
        v.positions = static_cast<int>(repo_.arena_positions(b.name).size());
    }
    v.stage_label = stage_label(b, v.live_days > 0 ? v.live_days - 1 : 0);
    return v;
}

std::vector<BotView> ArenaService::view() {
    const auto bots = repo_.arena_bots();
    std::vector<BotView> out;
    for (const auto& spec : roster()) {
        for (const auto& b : bots) {
            if (b.name == spec.name) out.push_back(view_of(b));
        }
    }
    return out;
}

std::string ArenaService::leaderboard(bool include_private) {
    auto bots = view();
    if (bots.empty()) return "The arena is warming up: the bots are being tested on historical prices. Ask again in a few minutes.";
    auto rank = [](const BotView& v) { return v.stage == "public" ? 0 : v.stage == "benchmark" ? 1 : v.stage == "proving" ? 2 : 3; };
    std::stable_sort(bots.begin(), bots.end(), [&](const BotView& a, const BotView& b) {
        return rank(a) != rank(b) ? rank(a) < rank(b) : a.live_return > b.live_return;
    });
    std::ostringstream t;
    t << pad("BOT", 11) << pad("STAGE", 19) << pad("VALUE", 11) << pad("RETURN", 9) << pad("vs ATLAS", 10) << "TRADES\n";
    int hidden = 0;
    for (const auto& v : bots) {
        const bool shown = v.stage == "public" || v.stage == "benchmark" || include_private;
        if (!shown) {
            ++hidden;
            continue;
        }
        const bool live = v.stage != "training";
        t << pad(v.name, 11) << pad(v.stage_label, 19) << pad(live ? format_money(v.value) : "-", 11)
          << pad(live ? signed_percent(v.live_return) : "-", 9)
          << pad(live && v.stage != "benchmark" ? signed_percent(v.vs_atlas) : "-", 10) << (live ? std::to_string(v.live_trades) : "-")
          << "\n";
    }
    std::ostringstream m;
    m << "**Strojanka Arena**: trading bots that each started with $1,000 of play money\n```\n" << t.str() << "```\n";
    if (hidden > 0) m << hidden << " more bot(s) are still in training or proving themselves in private.\n";
    m << "A bot trades in public only after passing a test on about 8 years of past prices and then doing well with live "
         "prices for " << kProvingDays << " trading days. Atlas simply holds every stock: the market to beat. "
         "Play money only, not investment advice.";
    return m.str();
}

std::string ArenaService::details(const std::string& name) {
    const Spec* spec = find_spec(name);
    if (!spec) {
        std::string names;
        for (const auto& s : roster()) names += (names.empty() ? "" : ", ") + s.name;
        return "No bot called `" + name + "`. The bots are: " + names + ".";
    }
    const auto bots = repo_.arena_bots();
    const auto it = std::find_if(bots.begin(), bots.end(), [&](const ArenaBotRecord& b) { return b.name == spec->name; });
    if (it == bots.end()) return "**" + spec->name + "** has not been tested yet. The arena starts within the hour.";
    const BotView v = view_of(*it);
    const auto bt = parse(it->backtest_json);
    std::ostringstream m;
    m << "**" << v.name << "**, named after the " << v.origin << ". Stage: **" << v.stage_label << "**\n" << v.idea << "\n\n";
    if (!it->backtest_json.empty()) {
        m << "**Historical test** (" << bt.value("from", std::string()) << " to " << bt.value("to", std::string()) << ", $1,000 start): "
          << format_money(bt.value("final_value", kStartingCash)) << " (" << signed_percent(v.bt_return) << ", "
          << signed_percent(v.bt_cagr) << " a year), worst fall " << format_percent(v.bt_drawdown, 0) << ", Sharpe "
          << format_double(v.bt_sharpe, 2);
        if (v.stage != "benchmark") {
            m << ", " << v.bt_trades << " completed trades, " << format_percent(bt.value("win_rate", 0.0), 0) << " of them profitable.\n"
              << (v.backtest_passed ? "Verdict: " : "Not passed: ") << v.verdict;
        }
        m << "\n*The historical test uses today's list of large stocks, which flatters every strategy (yesterday's winners "
             "are on it, the losers that fell off are not). Compare with Atlas, who shares the same advantage, and trust "
             "the live results more.*\n\n";
    }
    if (v.stage != "training") {
        m << "**Live** since " << it->live_started << ": " << format_money(v.value) << " (" << signed_percent(v.live_return) << ")";
        if (v.stage != "benchmark") m << ", " << signed_percent(v.vs_atlas) << " compared with Atlas";
        m << ", worst fall " << format_percent(v.live_drawdown, 1) << ", " << v.live_trades << " trades, cash "
          << format_money(it->cash) << ".\n";
        const auto positions = repo_.arena_positions(v.name);
        if (!positions.empty()) {
            m << "Holding:";
            for (const auto& p : positions) m << " " << p.symbol;
            m << "\n";
        }
        const auto trades = repo_.arena_trades(v.name, 8);
        if (!trades.empty()) {
            m << "Latest trades:\n";
            for (const auto& t : trades) {
                m << "- " << t.date << " " << (t.buy ? "bought " : "sold ") << t.symbol << " " << format_money(t.value);
                if (!t.buy) m << " (" << format_signed_money(t.profit) << ")";
                m << "\n";
            }
        }
    }
    return m.str();
}

}  // namespace strojanka::arena
