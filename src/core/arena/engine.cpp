#include "core/arena/engine.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "common/strings.h"
#include "core/analysis/analyzer.h"
#include "core/analysis/calibrator.h"
#include "core/analysis/indicators.h"

namespace strojanka::arena {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kSwingStopLoss = 0.08;
constexpr int kSwingMaxDays = 40;
constexpr int kMeanReversionMaxDays = 10;

// Gate for the historical test.
constexpr int kMinClosedTrades = 10;
constexpr double kMinSharpe = 0.6;
constexpr double kMaxDrawdown = 0.35;

std::vector<double> sma(const std::vector<double>& c, int n) {
    std::vector<double> out(c.size(), kNaN);
    double sum = 0;
    for (size_t i = 0; i < c.size(); ++i) {
        sum += c[i];
        if (i >= static_cast<size_t>(n)) sum -= c[i - n];
        if (i + 1 >= static_cast<size_t>(n)) out[i] = sum / n;
    }
    return out;
}

std::vector<double> window_extreme(const std::vector<Bar>& bars, int n, bool high) {
    std::vector<double> out(bars.size(), kNaN);
    for (size_t i = static_cast<size_t>(n); i < bars.size(); ++i) {
        double v = high ? -1e300 : 1e300;
        for (size_t k = i - n; k < i; ++k) v = high ? std::max(v, bars[k].high) : std::min(v, bars[k].low);
        out[i] = v;
    }
    return out;
}

bool ok(double v) { return !std::isnan(v); }

const std::vector<double>& sma_of(const SymbolData& s, int n) {
    switch (n) {
        case 5: return s.sma5;
        case 20: return s.sma20;
        case 50: return s.sma50;
        case 100: return s.sma100;
        default: return s.sma200;
    }
}

int max_positions(const Spec& spec, const Universe& u) {
    return spec.kind == Kind::BuyHold ? static_cast<int>(u.symbols.size()) : kMaxPositions;
}

double close_on_or_before(const SymbolData& s, int day) {
    for (int d = day; d >= 0 && d > day - 15; --d) {
        if (s.at[d] >= 0) return s.bars[s.at[d]].close;
    }
    return 0;
}

const SymbolData* find_symbol(const Universe& u, const std::string& symbol) {
    for (const auto& s : u.symbols) {
        if (s.symbol == symbol) return &s;
    }
    return nullptr;
}

bool enters(const Spec& spec, const SymbolData& s, int bi) {
    const double close = s.bars[bi].close;
    switch (spec.kind) {
        case Kind::Swing: return s.direction[bi] < 0 && s.support_score[bi] >= spec.c;
        case Kind::Trend: {
            const double fast = sma_of(s, spec.a)[bi], slow = sma_of(s, spec.b)[bi];
            return ok(fast) && ok(slow) && fast > slow && close > slow;
        }
        case Kind::Breakout: {
            const double high = (spec.a == 20 ? s.high20 : s.high55)[bi];
            return ok(high) && close > high;
        }
        case Kind::MeanReversion: return ok(s.rsi2[bi]) && ok(s.sma200[bi]) && s.rsi2[bi] < spec.a && close > s.sma200[bi];
        case Kind::Oracle: return ok(s.oracle_up[bi]) && s.oracle_up[bi] >= spec.c;
        default: return false;
    }
}

bool keeps(const Spec& spec, const SymbolData& s, int bi, const Position& p, int day) {
    const double close = s.bars[bi].close;
    switch (spec.kind) {
        case Kind::Swing: {
            const bool peak = s.direction[bi] > 0 && s.peak_score[bi] >= spec.c;
            const bool stopped = p.shares > 0 && close <= (p.cost / p.shares) * (1 - kSwingStopLoss);
            return !peak && !stopped && day - p.entry_day < kSwingMaxDays;
        }
        case Kind::Trend: return enters(spec, s, bi);
        case Kind::Breakout: {
            const double low = (spec.b == 10 ? s.low10 : s.low20)[bi];
            return !ok(low) || close >= low;
        }
        case Kind::MeanReversion: {
            if (day - p.entry_day >= kMeanReversionMaxDays) return false;
            if (spec.b == 0) return !(ok(s.sma5[bi]) && close > s.sma5[bi]);
            return !(ok(s.rsi2[bi]) && s.rsi2[bi] > 70);
        }
        case Kind::Oracle: {
            // Hold while Pythia still sees a fair chance, for at most the 20 days its prediction covers.
            const bool stopped = p.shares > 0 && close <= (p.cost / p.shares) * (1 - kSwingStopLoss);
            return !stopped && day - p.entry_day < 20 && (!ok(s.oracle_up[bi]) || s.oracle_up[bi] >= spec.c - 0.1);
        }
        default: return true;
    }
}

double score(const Spec& spec, const SymbolData& s, int bi) {
    switch (spec.kind) {
        case Kind::Swing: return s.support_score[bi];
        case Kind::Trend: return sma_of(s, spec.a)[bi] / sma_of(s, spec.b)[bi];
        case Kind::Breakout: return s.bars[bi].close / (spec.a == 20 ? s.high20 : s.high55)[bi];
        case Kind::MeanReversion: return -s.rsi2[bi];
        case Kind::Momentum: return (spec.a == 126 ? s.mom126 : s.mom252)[bi];
        case Kind::Oracle: return s.oracle_up[bi];
        default: return 0;
    }
}

void decide(const Spec& spec, const Universe& u, int day, State& state) {
    if (spec.kind == Kind::BuyHold) {
        if (state.positions.empty() && state.orders.empty() && state.cash > 1) {
            for (const auto& s : u.symbols) {
                if (s.at[day] >= 0) state.orders.push_back({s.symbol, true});
            }
        }
        return;
    }
    const int slots = max_positions(spec, u);
    if (spec.kind == Kind::Momentum) {
        // Rebalance on the first trading day of each month.
        if (day == 0 || u.calendar[day].substr(0, 7) == u.calendar[day - 1].substr(0, 7)) return;
        std::vector<std::pair<double, std::string>> ranked;
        for (const auto& s : u.symbols) {
            const int bi = s.at[day];
            if (bi < 0) continue;
            const double m = score(spec, s, bi);
            if (ok(m) && m > 0) ranked.emplace_back(m, s.symbol);
        }
        std::sort(ranked.rbegin(), ranked.rend());
        std::vector<std::string> target;
        for (size_t i = 0; i < ranked.size() && static_cast<int>(target.size()) < slots; ++i) target.push_back(ranked[i].second);
        for (const auto& [symbol, pos] : state.positions) {
            if (std::find(target.begin(), target.end(), symbol) == target.end()) state.orders.push_back({symbol, false});
        }
        for (const auto& symbol : target) {
            if (!state.positions.count(symbol)) state.orders.push_back({symbol, true});
        }
        return;
    }

    int holding = 0;
    for (const auto& [symbol, pos] : state.positions) {
        const SymbolData* s = find_symbol(u, symbol);
        ++holding;
        if (!s || s->at[day] < 0) continue;
        if (!keeps(spec, *s, s->at[day], pos, day)) {
            state.orders.push_back({symbol, false});
            --holding;
        }
    }
    const int free = slots - holding;
    if (free <= 0) return;
    std::vector<std::pair<double, std::string>> candidates;
    for (const auto& s : u.symbols) {
        const int bi = s.at[day];
        if (bi < 0 || state.positions.count(s.symbol)) continue;
        if (enters(spec, s, bi)) candidates.emplace_back(score(spec, s, bi), s.symbol);
    }
    std::sort(candidates.rbegin(), candidates.rend());
    for (int i = 0; i < free && i < static_cast<int>(candidates.size()); ++i) state.orders.push_back({candidates[i].second, true});
}

}  // namespace

const std::vector<Spec>& roster() {
    static const std::vector<Spec> specs = {
        {"Atlas", "Titan who carries the world", "Buys every stock on day one and holds them all: the market itself, the benchmark everyone must beat.", Kind::BuyHold},
        {"Athena", "Greek goddess of wisdom", "Buys at Strojanka's SUPPORT signals and sells at its PEAK signals (8% stop-loss, at most 40 days).", Kind::Swing, 0, 0, 0.65},
        {"Minerva", "Roman goddess of wisdom", "Like Athena, but only acts on the strongest signals.", Kind::Swing, 0, 0, 0.75},
        {"Ares", "Greek god of war", "Follows trends: holds a stock while its 20-day average is above its 100-day average.", Kind::Trend, 20, 100},
        {"Mars", "Roman god of war", "Follows long trends: holds while the 50-day average is above the 200-day average.", Kind::Trend, 50, 200},
        {"Hermes", "Greek messenger god, the fastest", "Momentum: each month holds the 5 stocks that rose most over 6 months.", Kind::Momentum, 126},
        {"Mercurius", "Roman messenger god", "Momentum: each month holds the 5 strongest stocks of the last year (skipping the last month).", Kind::Momentum, 252},
        {"Artemis", "Greek goddess of the hunt", "Breakouts: buys a stock closing above its 20-day high, sells below its 10-day low.", Kind::Breakout, 20, 10},
        {"Diana", "Roman goddess of the hunt", "Slow breakouts: buys above the 55-day high, sells below the 20-day low.", Kind::Breakout, 55, 20},
        {"Nemesis", "Greek goddess of retribution for excess", "Buys sharp 2-day sell-offs in stocks above their 200-day average, sells on the rebound.", Kind::MeanReversion, 10, 0},
        {"Iustitia", "Roman goddess of justice", "Buys only extreme sell-offs in rising stocks and waits for a full recovery.", Kind::MeanReversion, 5, 1},
        {"Pythia", "oracle of Delphi", "Trades on the built-in AI model Pythia: buys the stocks it gives at least a 50% chance of a clear rise, for up to 20 days.", Kind::Oracle, 0, 0, 0.50},
        {"Sibylla", "Roman oracle, the Sibyl", "Like Pythia, but only buys when the AI model gives a rise at least a 55% chance.", Kind::Oracle, 0, 0, 0.55},
    };
    return specs;
}

const Spec* find_spec(const std::string& name) {
    const std::string key = to_lower(trim(name));
    for (const auto& s : roster()) {
        if (to_lower(s.name) == key) return &s;
    }
    return nullptr;
}

int Universe::day_of(const std::string& date) const {
    const auto it = std::lower_bound(calendar.begin(), calendar.end(), date);
    return it != calendar.end() && *it == date ? static_cast<int>(it - calendar.begin()) : -1;
}

Universe build_universe(const std::map<std::string, std::vector<Bar>>& bars_by_symbol) {
    Universe u;
    std::map<std::string, int> counts;
    for (const auto& [symbol, bars] : bars_by_symbol) {
        for (const auto& b : bars) counts[b.date]++;
    }
    const int need = std::max<int>(1, static_cast<int>(bars_by_symbol.size()) / 2);
    for (const auto& [date, n] : counts) {
        if (n >= need) u.calendar.push_back(date);
    }
    for (const auto& [symbol, bars] : bars_by_symbol) {
        if (bars.size() < 60) continue;
        SymbolData s;
        s.symbol = symbol;
        s.bars = bars;
        s.at.assign(u.calendar.size(), -1);
        for (size_t i = 0; i < bars.size(); ++i) {
            const int d = u.day_of(bars[i].date);
            if (d >= 0) s.at[d] = static_cast<int>(i);
        }
        const auto closes = analysis::closes_of(bars);
        s.sma5 = sma(closes, 5);
        s.sma20 = sma(closes, 20);
        s.sma50 = sma(closes, 50);
        s.sma100 = sma(closes, 100);
        s.sma200 = sma(closes, 200);
        s.rsi2 = analysis::rsi(closes, 2);
        s.high20 = window_extreme(bars, 20, true);
        s.high55 = window_extreme(bars, 55, true);
        s.low10 = window_extreme(bars, 10, false);
        s.low20 = window_extreme(bars, 20, false);
        s.mom126.assign(bars.size(), kNaN);
        s.mom252.assign(bars.size(), kNaN);
        for (size_t i = 0; i < bars.size(); ++i) {
            if (i >= 126) s.mom126[i] = closes[i] / closes[i - 126] - 1;
            if (i >= 252) s.mom252[i] = closes[i - 21] / closes[i - 252] - 1;
        }
        // Swing settings come from the first year only, so the old-data test never uses later knowledge.
        const std::vector<Bar> first_year(bars.begin(), bars.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(bars.size(), 260)));
        const auto states = analysis::analyze_series(closes, analysis::default_params(first_year));
        s.peak_score.resize(bars.size());
        s.support_score.resize(bars.size());
        s.direction.resize(bars.size());
        for (size_t i = 0; i < bars.size(); ++i) {
            s.peak_score[i] = states[i].valid ? states[i].peak_score : 0;
            s.support_score[i] = states[i].valid ? states[i].support_score : 0;
            s.direction[i] = states[i].valid ? states[i].direction : 0;
        }
        s.oracle_up.assign(bars.size(), kNaN);
        s.oracle_down.assign(bars.size(), kNaN);
        u.symbols.push_back(std::move(s));
    }
    return u;
}

double value_at(const Universe& u, int day, const State& state) {
    double v = state.cash;
    for (const auto& [symbol, pos] : state.positions) {
        if (const SymbolData* s = find_symbol(u, symbol)) v += pos.shares * close_on_or_before(*s, day);
    }
    return v;
}

double step(const Spec& spec, const Universe& u, int day, State& state, std::vector<Fill>& fills) {
    const double equity_before = day > 0 ? value_at(u, day - 1, state) : state.cash;
    const int slots = max_positions(spec, u);
    std::vector<Order> orders;
    orders.swap(state.orders);

    for (const auto& o : orders) {  // sells first, so their cash can fund today's buys
        if (o.buy) continue;
        const SymbolData* s = find_symbol(u, o.symbol);
        auto it = state.positions.find(o.symbol);
        if (!s || it == state.positions.end() || s->at[day] < 0) continue;
        const Bar& bar = s->bars[s->at[day]];
        const double price = (bar.open > 0 ? bar.open : bar.close) * (1 - kSlippage);
        const double proceeds = it->second.shares * price;
        fills.push_back({o.symbol, false, it->second.shares, price, proceeds, proceeds - it->second.cost, day});
        state.cash += proceeds;
        state.positions.erase(it);
    }
    int buys_left = 0;
    for (const auto& o : orders) buys_left += o.buy ? 1 : 0;
    for (const auto& o : orders) {
        if (!o.buy) continue;
        const SymbolData* s = find_symbol(u, o.symbol);
        const int remaining = buys_left--;
        if (!s || s->at[day] < 0 || state.positions.count(o.symbol)) continue;
        const double amount = std::min(state.cash / remaining, equity_before / slots);
        if (amount < 1.0) continue;
        const Bar& bar = s->bars[s->at[day]];
        const double price = (bar.open > 0 ? bar.open : bar.close) * (1 + kSlippage);
        const double shares = amount / price;
        state.cash -= amount;
        state.positions[o.symbol] = {shares, amount, day};
        fills.push_back({o.symbol, true, shares, price, amount, 0, day});
    }

    const double value = value_at(u, day, state);
    state.peak = std::max(state.peak, value);
    decide(spec, u, day, state);
    return value;
}

Metrics measure(const std::vector<double>& equity, const std::vector<Fill>& fills, const std::string& from, const std::string& to) {
    Metrics m;
    m.from = from;
    m.to = to;
    m.days = static_cast<int>(equity.size());
    if (equity.empty()) return m;
    m.final_value = equity.back();
    m.total_return = m.final_value / kStartingCash - 1;
    m.cagr = std::pow(std::max(m.final_value, 1e-9) / kStartingCash, 252.0 / std::max(1, m.days)) - 1;
    double peak = kStartingCash, mean = 0, sq = 0;
    std::vector<double> returns;
    for (size_t i = 0; i < equity.size(); ++i) {
        peak = std::max(peak, equity[i]);
        m.max_drawdown = std::max(m.max_drawdown, 1 - equity[i] / peak);
        const double prev = i == 0 ? kStartingCash : equity[i - 1];
        returns.push_back(equity[i] / prev - 1);
    }
    for (double r : returns) mean += r;
    mean /= static_cast<double>(returns.size());
    for (double r : returns) sq += (r - mean) * (r - mean);
    const double sd = returns.size() > 1 ? std::sqrt(sq / static_cast<double>(returns.size() - 1)) : 0;
    m.sharpe = sd > 0 ? mean / sd * std::sqrt(252.0) : 0;
    const size_t n = equity.size();
    const size_t cut[4] = {0, n / 3, 2 * n / 3, n - 1};
    for (int k = 0; k < 3; ++k) {
        const double start = k == 0 ? kStartingCash : equity[cut[k]];
        m.thirds[k] = equity[cut[k + 1]] / start - 1;
    }
    int wins = 0;
    for (const auto& f : fills) {
        ++m.trades;
        if (!f.buy) {
            ++m.closed_trades;
            wins += f.profit > 0;
        }
    }
    m.win_rate = m.closed_trades ? static_cast<double>(wins) / m.closed_trades : 0;
    return m;
}

BacktestResult backtest(const Spec& spec, const Universe& u, int from_day) {
    BacktestResult r;
    const int last = static_cast<int>(u.calendar.size()) - 1;
    if (from_day >= last) {
        r.verdict = "not enough price history yet";
        return r;
    }
    State state;
    std::vector<Fill> fills;
    std::vector<double> equity;
    for (int day = from_day; day <= last; ++day) equity.push_back(step(spec, u, day, state, fills));
    r.metrics = measure(equity, fills, u.calendar[from_day], u.calendar[last]);
    const Metrics& m = r.metrics;
    if (spec.kind == Kind::BuyHold) {
        r.passed = true;
        r.verdict = "benchmark";
        return r;
    }
    std::vector<std::string> problems;
    if (m.closed_trades < kMinClosedTrades) problems.push_back("only " + std::to_string(m.closed_trades) + " completed trades, too few to judge");
    if (m.sharpe < kMinSharpe) problems.push_back("returns too small for their swings (Sharpe " + format_double(m.sharpe, 2) + ", needs " + format_double(kMinSharpe, 1) + ")");
    if (m.max_drawdown > kMaxDrawdown) problems.push_back("worst fall " + format_percent(m.max_drawdown, 0) + " (limit " + format_percent(kMaxDrawdown, 0) + ")");
    for (int k = 0; k < 3; ++k) {
        if (m.thirds[k] <= 0) {
            problems.push_back("lost money in the " + std::string(k == 0 ? "first" : k == 1 ? "middle" : "last") + " third of the period");
        }
    }
    r.passed = problems.empty();
    if (r.passed) {
        r.verdict = "passed: profitable in every third, Sharpe " + format_double(m.sharpe, 2) + ", worst fall " + format_percent(m.max_drawdown, 0);
    } else {
        r.verdict = problems.front();
        for (size_t i = 1; i < problems.size(); ++i) r.verdict += "; " + problems[i];
    }
    return r;
}

}  // namespace strojanka::arena
