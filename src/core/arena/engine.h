#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

#include "core/models.h"

// The arena: paper-trading bots, each running one strategy on the same list of stocks with
// $1,000 of play money. The same step() function drives the historical backtest and live trading,
// so a bot is judged identically on old and on new data.
namespace strojanka::arena {

constexpr double kStartingCash = 1000.0;
constexpr double kSlippage = 0.001;  // 0.1% worse price on every fill, for spreads and fees
constexpr int kMaxPositions = 5;
constexpr int kWarmupDays = 275;     // indicators need about a year of history

enum class Kind { BuyHold, Swing, Trend, Momentum, Breakout, MeanReversion, Oracle };

struct Spec {
    std::string name;    // Greek or Latin name
    std::string origin;  // who the name belongs to
    std::string idea;    // the strategy in one sentence
    Kind kind;
    int a = 0, b = 0;    // strategy parameters (see engine.cpp)
    double c = 0;
};

const std::vector<Spec>& roster();               // Atlas (benchmark) first
const Spec* find_spec(const std::string& name);  // case-insensitive

// Daily prices of one stock aligned to the shared trading calendar, with precomputed indicators.
struct SymbolData {
    std::string symbol;
    std::vector<Bar> bars;
    std::vector<int> at;  // calendar day -> bar index, or -1 if the stock has no bar that day
    std::vector<double> sma5, sma20, sma50, sma100, sma200, rsi2;
    std::vector<double> high20, high55, low10, low20;  // of the previous N bars (not including today)
    std::vector<double> mom126, mom252;                // 6-month return; 12-month return skipping the last month
    std::vector<double> peak_score, support_score;     // Strojanka swing analysis
    std::vector<int> direction;
    // Pythia's chance of a rise / fall (see ml/pythia.h); NaN where it has no prediction.
    std::vector<double> oracle_up, oracle_down;
};

struct Universe {
    std::vector<std::string> calendar;  // trading days, oldest first
    std::vector<SymbolData> symbols;
    int day_of(const std::string& date) const;  // -1 if not a trading day
};

// Builds the calendar (days on which at least half of the stocks traded) and the indicators.
Universe build_universe(const std::map<std::string, std::vector<Bar>>& bars_by_symbol);

struct Position {
    double shares = 0;
    double cost = 0;  // money paid
    int entry_day = 0;
};

struct Order {
    std::string symbol;
    bool buy = true;
};

struct Fill {
    std::string symbol;
    bool buy = true;
    double shares = 0, price = 0, value = 0;
    double profit = 0;  // sells only: received minus cost
    int day = 0;
};

struct State {
    double cash = kStartingCash;
    std::map<std::string, Position> positions;
    std::vector<Order> orders;  // decided at a close, filled at the next day's open
    double peak = kStartingCash;
};

// One trading day: fill pending orders at today's open, value the portfolio at the close, then
// decide orders for tomorrow using only data up to today's close. Returns the closing value.
double step(const Spec& spec, const Universe& u, int day, State& state, std::vector<Fill>& fills);
double value_at(const Universe& u, int day, const State& state);

struct Metrics {
    double final_value = kStartingCash;
    double total_return = 0;
    double cagr = 0;
    double max_drawdown = 0;
    double sharpe = 0;
    double win_rate = 0;
    int trades = 0;         // buys + sells
    int closed_trades = 0;  // sells
    std::array<double, 3> thirds{};  // return in each third of the period
    int days = 0;
    std::string from, to;
};

Metrics measure(const std::vector<double>& equity, const std::vector<Fill>& fills, const std::string& from, const std::string& to);

struct BacktestResult {
    Metrics metrics;
    bool passed = false;
    std::string verdict;  // why it passed or failed, in plain words
};

// Simulates the spec from `from_day` to the last calendar day with $1,000.
BacktestResult backtest(const Spec& spec, const Universe& u, int from_day = kWarmupDays);

}  // namespace strojanka::arena
