#include "core/portfolio.h"

#include <algorithm>

namespace strojanka {

Position compute_position(const std::vector<TransactionRecord>& transactions) {
    Position p;
    for (const auto& t : transactions) {
        if (t.side == TxSide::Buy) {
            p.shares += t.shares;
            p.cost_basis += t.total;
            p.total_invested += t.total;
        } else {
            const double sold = std::min(t.shares, p.shares);
            const double cost_of_sold = p.shares > kShareEpsilon ? p.cost_basis * (sold / p.shares) : 0.0;
            p.realized_profit += t.total - cost_of_sold;
            p.total_received += t.total;
            p.shares_sold += sold;
            p.shares -= sold;
            p.cost_basis -= cost_of_sold;
            if (p.shares <= kShareEpsilon) {
                p.shares = 0;
                p.cost_basis = 0;
            }
        }
    }
    return p;
}

}  // namespace strojanka
