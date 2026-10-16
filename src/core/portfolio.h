#pragma once

#include <string>
#include <vector>

#include "core/models.h"

namespace strojanka {

constexpr double kShareEpsilon = 1e-9;

// Holdings for one user and one stock, using the average-cost method:
// each sale is matched against the average price paid for the shares still held.
struct Position {
    double shares = 0;            // shares currently held
    double cost_basis = 0;        // money paid for the shares currently held
    double total_invested = 0;    // sum of all BUY totals
    double total_received = 0;    // sum of all SELL totals
    double realized_profit = 0;   // received - cost of the shares sold
    double shares_sold = 0;

    double average_cost() const { return shares > kShareEpsilon ? cost_basis / shares : 0.0; }
    double market_value(double price) const { return shares * price; }
    double unrealized_profit(double price) const { return market_value(price) - cost_basis; }
    bool has_shares() const { return shares > kShareEpsilon; }
};

// Applies one transaction of this stock; returns the profit it realized (0 for a buy).
double apply_transaction(Position& position, const TransactionRecord& tx);

// Transactions must be in chronological order (Repository::transactions returns them that way).
Position compute_position(const std::vector<TransactionRecord>& transactions);

// One row of a user's trading history.
struct TradeEntry {
    TransactionRecord tx;
    double shares = 0;           // shares actually traded (a sale is capped at what was held)
    double price = 0;            // per share: total / shares
    double realized_profit = 0;  // sales only: received minus the average cost of the shares sold
    double held_after = 0;       // shares of this stock held after the trade
};

// Replays transactions of any number of stocks (chronological) with the same average-cost rules
// as compute_position, so the history and the portfolio always agree.
std::vector<TradeEntry> trade_history(const std::vector<TransactionRecord>& transactions);

}  // namespace strojanka
