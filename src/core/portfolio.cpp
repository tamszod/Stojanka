#include "core/portfolio.h"

#include <algorithm>
#include <map>

namespace strojanka {

double apply_transaction(Position& p, const TransactionRecord& t) {
    if (t.side == TxSide::Buy) {
        p.shares += t.shares;
        p.cost_basis += t.total;
        p.total_invested += t.total;
        return 0;
    }
    const double sold = std::min(t.shares, p.shares);
    const double cost_of_sold = p.shares > kShareEpsilon ? p.cost_basis * (sold / p.shares) : 0.0;
    const double profit = t.total - cost_of_sold;
    p.realized_profit += profit;
    p.total_received += t.total;
    p.shares_sold += sold;
    p.shares -= sold;
    p.cost_basis -= cost_of_sold;
    if (p.shares <= kShareEpsilon) {
        p.shares = 0;
        p.cost_basis = 0;
    }
    return profit;
}

Position compute_position(const std::vector<TransactionRecord>& transactions) {
    Position p;
    for (const auto& t : transactions) apply_transaction(p, t);
    return p;
}

std::vector<TradeEntry> trade_history(const std::vector<TransactionRecord>& transactions) {
    std::map<std::string, Position> positions;
    std::vector<TradeEntry> out;
    out.reserve(transactions.size());
    for (const auto& t : transactions) {
        Position& p = positions[t.symbol];
        TradeEntry e;
        e.tx = t;
        e.shares = t.side == TxSide::Buy ? t.shares : std::min(t.shares, p.shares);
        e.price = e.shares > kShareEpsilon ? t.total / e.shares : 0.0;
        e.realized_profit = apply_transaction(p, t);
        e.held_after = p.shares;
        out.push_back(e);
    }
    return out;
}

}  // namespace strojanka
