#include "core/commands.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

#include "common/strings.h"
#include "core/markets.h"
#include "core/messages.h"
#include "core/admin_service.h"
#include "core/arena/arena_service.h"
#include "core/ml/pythia.h"
#include "core/portfolio.h"
#include "core/screener.h"
#include "core/version.h"

namespace strojanka {

namespace {

CommandReply fail(const std::string& text) { return {false, text}; }

bool parse_symbol(const std::string& input, std::string& symbol, CommandReply& error) {
    symbol = normalize_symbol(input);
    if (!is_valid_symbol(symbol)) {
        error = fail("`" + input + "` is not a valid ticker symbol. Use symbols like `FCX`, `BRK-B`, `SPY` or `CSPX.L`.");
        return false;
    }
    return true;
}

// Pads to a width in characters, not bytes, so columns with € or £ line up.
std::string pad(const std::string& s, size_t width) {
    const size_t chars = utf8_length(s);
    return chars >= width ? s : s + std::string(width - chars, ' ');
}

}  // namespace

CommandReply CommandService::help() const {
    std::ostringstream m;
    m << "**Strojanka " << STROJANKA_VERSION << "** watches your stocks and tells you when they reach a PEAK "
      << "(time to sell) or a SUPPORT level (time to buy).\n\n"
      << "`/status` - your watched stocks: price, trend, value | `/portfolio` - what you hold, with profit\n"
      << "`/profit` - profit per stock | `/history` - every buy and sell (`stock:FCX` for one)\n"
      << "`/monitor stock:FCX` - watch a stock or ETF and get a report; outside the US add the exchange ending, "
      << "e.g. `CSPX.L` (London), `OTP.BD` (Budapest)\n"
      << "`/remove stock:FCX` - stop watching (only when you hold none)\n"
      << "`/buy stock:FCX shares:0.858291457 paid:59.78$` / `/sell ... received:67.87$` - record a trade (total amounts)\n"
      << "`/report stock:FCX` - analysis | `/news stock:FCX` - headlines | `/suggest` - stocks with proven signals\n"
      << "`/market` - exchanges: ticker ending, opening hours, open now?\n"
      << "`/arena` - paper-trading bots | `/pythia` - the built-in AI model\n"
      << "`/politicians` - US Congress stock trades; `monitor politician:Pelosi` for a DM on a new one\n"
      << "`/admin` - admin panel, backups (admins only)\n\n"
      << "You can also just type commands here, e.g. `buy FCX 0.858291457 59.78$` or `status`. "
      << "Alerts arrive as direct messages; PEAK only for stocks you hold.\n"
      << "*Signals are statistical estimates, not financial advice.*";
    return {true, m.str()};
}

bool CommandService::ensure_stock(const std::string& symbol, std::string* report_text, std::string* error) {
    if (repo_.get_stock(symbol) && !report_text) return true;
    auto report = monitor_.refresh(symbol, false, error);
    if (!report) return false;
    if (report_text) *report_text = format_report(*report);
    return true;
}

CommandReply CommandService::lookup_failed(const std::string& intro, const std::string& symbol, const std::string& error,
                                           const std::string& example_command) {
    std::string text = intro + ": " + error;
    if (error.find("not found") == std::string::npos && error.find("No data found") == std::string::npos) return fail(text);

    // HEAL.G: ".G" is no exchange ending, and searching finds neither it nor the London HEAL.L,
    // so also try the base ticker on the main exchanges.
    const auto dot = symbol.rfind('.');
    const std::string base = dot == std::string::npos ? symbol : symbol.substr(0, dot);
    const std::string ending = dot == std::string::npos ? "" : symbol.substr(dot);
    std::vector<SymbolMatch> matches;
    auto add = [&](SymbolMatch m) {
        if (m.symbol == symbol || matches.size() >= 5) return;
        for (const auto& x : matches) if (x.symbol == m.symbol) return;
        matches.push_back(std::move(m));
    };
    std::vector<SymbolMatch> searched = monitor_.search_symbols(symbol);
    if (!ending.empty() || searched.empty()) {
        for (auto& m : monitor_.probe_listings(base)) add(std::move(m));
    }
    for (auto& m : searched) add(std::move(m));

    static const char* const kEndings[] = {".L",  ".DE", ".F",  ".AS", ".PA", ".MI", ".SW", ".MC", ".BR", ".LS", ".VI",
                                           ".ST", ".CO", ".OL", ".HE", ".IR", ".WA", ".PR", ".BD", ".TO", ".V",  ".AX",
                                           ".HK", ".T",  ".KS", ".SS", ".SZ", ".NS", ".BO", ".SA", ".MX", ".TA", ".SI"};
    bool known_ending = ending.empty();
    for (const char* e : kEndings) known_ending = known_ending || ending == e;
    if (!known_ending) {
        text += ". `" + ending + "` isn't an exchange ending Yahoo Finance uses (London is `.L`, Xetra `.DE`, Amsterdam "
                "`.AS`, Budapest `.BD`)";
    }
    if (matches.empty()) return fail(text + ".");
    text += ". Did you mean one of these?\n";
    for (const auto& m : matches) {
        text += "- `" + m.symbol + "` " + m.name + " (" + (m.type == "ETF" ? "ETF" : "stock");
        if (!m.exchange.empty()) text += ", " + m.exchange;
        text += ")\n";
    }
    text += "Use the exact symbol, for example `" + example_command + matches.front().symbol +
            "` (listings outside the US end in their exchange, like `.L` for London or `.DE` for Xetra).";
    return fail(text);
}

CommandReply CommandService::status(const std::string& user) {
    const auto symbols = repo_.user_watchlist(user);
    if (symbols.empty()) return {true, "You are not watching any stocks yet. Start with `/monitor stock:FCX`."};

    std::ostringstream table;
    std::map<std::string, std::pair<double, double>> totals;  // currency -> (value, cost)
    std::string currency = "USD";
    table << pad("STOCK", 9) << pad("COMPANY", 15) << pad("PRICE", 12) << pad("TREND", 12) << pad("SHARES", 14)
          << pad("VALUE", 12) << "P/L\n";
    for (const auto& sym : symbols) {
        const auto stock = repo_.get_stock(sym);
        const Position pos = compute_position(repo_.transactions(user, sym));
        const double price = stock ? stock->last_price : 0;
        const std::string cur = stock ? stock->currency : "USD";
        currency = cur;
        const std::string company = stock && !stock->name.empty() ? short_company_name(stock->name, 14) : "-";
        table << pad(sym, 9) << pad(company, 15) << pad(stock ? format_money(price, cur) : "?", 12)
              << pad(stock ? stock->trend : "?", 12);
        if (pos.has_shares()) {
            const double value = pos.market_value(price);
            totals[cur].first += value;
            totals[cur].second += pos.cost_basis;
            table << pad(format_shares(pos.shares), 14) << pad(format_money(value, cur), 12)
                  << format_signed_money(pos.unrealized_profit(price), cur);
        } else {
            table << pad("-", 14) << pad("-", 12) << "-";
        }
        table << "\n";
    }
    if (totals.empty()) totals[currency] = {0, 0};
    auto balance = [](const std::string& cur, double value, double cost) {
        return format_money(value, cur) + " invested (paid " + format_money(cost, cur) + ", unrealized " +
               format_signed_money(value - cost, cur) + ")";
    };
    std::ostringstream m;
    m << "**Your watched stocks and ETFs**\n```\n" << table.str() << "```\n";
    if (totals.size() == 1) {
        const auto& [cur, t] = *totals.begin();
        m << "**Balance:** " << balance(cur, t.first, t.second);
    } else {
        m << "**Balance** per currency (not converted):";
        for (const auto& [cur, t] : totals) m << "\n- " << cur << ": " << balance(cur, t.first, t.second);
    }
    return {true, m.str()};
}

CommandReply CommandService::profit(const std::string& user, const std::string& symbol_input) {
    std::vector<std::string> symbols;
    if (!trim(symbol_input).empty()) {
        std::string symbol;
        CommandReply err;
        if (!parse_symbol(symbol_input, symbol, err)) return err;
        symbols.push_back(symbol);
    } else {
        symbols = repo_.traded_symbols(user);
    }
    if (symbols.empty()) return {true, "You have no recorded trades yet. Record one with `/buy`."};

    std::ostringstream table;
    std::map<std::string, std::pair<double, double>> totals;  // currency -> (realized, unrealized)
    table << pad("STOCK", 9) << pad("REALIZED", 13) << pad("UNREALIZED", 13) << "TOTAL\n";
    for (const auto& sym : symbols) {
        const auto txs = repo_.transactions(user, sym);
        if (txs.empty()) continue;
        const Position pos = compute_position(txs);
        const auto stock = repo_.get_stock(sym);
        const std::string cur = stock ? stock->currency : "USD";
        const double u = pos.has_shares() && stock ? pos.unrealized_profit(stock->last_price) : 0;
        totals[cur].first += pos.realized_profit;
        totals[cur].second += u;
        table << pad(sym, 9) << pad(format_signed_money(pos.realized_profit, cur), 13)
              << pad(pos.has_shares() ? format_signed_money(u, cur) : "-", 13)
              << format_signed_money(pos.realized_profit + u, cur) << "\n";
    }
    if (totals.empty()) return {true, "You have no recorded trades for " + symbols.front() + "."};
    auto summary = [](const std::string& cur, double realized, double unrealized) {
        return "**Realized:** " + format_signed_money(realized, cur) + " | **Unrealized:** " +
               format_signed_money(unrealized, cur) + " | **Total:** " + format_signed_money(realized + unrealized, cur);
    };
    std::ostringstream m;
    m << "**Profit** (realized = money received from sales minus what those shares cost)\n```\n" << table.str() << "```\n";
    if (totals.size() == 1) {
        const auto& [cur, t] = *totals.begin();
        m << summary(cur, t.first, t.second);
    } else {
        m << "Per currency (not converted):";
        for (const auto& [cur, t] : totals) m << "\n- " << cur << ": " << summary(cur, t.first, t.second);
    }
    return {true, m.str()};
}

namespace {

std::string signed_percent(double fraction) { return (fraction > 0.0005 ? "+" : "") + format_percent(fraction, 1); }

// "2026-10-16T10:46:03Z" -> "2026-10-16 10:46 UTC"
std::string readable_time(const std::string& iso) {
    if (iso.size() < 16) return iso;
    std::string s = iso.substr(0, 16);
    s[10] = ' ';
    return s + " UTC";
}

constexpr size_t kReplyBudget = 1900;  // Discord cuts messages at 2000 characters

}  // namespace

CommandReply CommandService::portfolio(const std::string& user) {
    std::map<std::string, std::vector<TransactionRecord>> by_symbol;
    for (auto& t : repo_.transactions(user)) by_symbol[t.symbol].push_back(std::move(t));
    if (by_symbol.empty()) {
        return {true, "Your portfolio is empty. Record a purchase with `/buy stock:FCX shares:2 paid:150$` "
                      "(`paid` is the total you paid)."};
    }

    struct Holding {
        std::string symbol, currency;
        Position pos;
        double price = 0;
        bool priced = false;
    };
    struct Totals {
        double value = 0, cost = 0, realized = 0;
        int holdings = 0;
    };
    std::vector<Holding> held;
    std::map<std::string, Totals> totals;  // per currency
    std::string oldest_price;
    for (const auto& [sym, txs] : by_symbol) {
        Holding h;
        h.symbol = sym;
        h.pos = compute_position(txs);
        const auto stock = repo_.get_stock(sym);
        h.currency = stock ? stock->currency : "USD";
        h.priced = stock && stock->last_price > 0;
        h.price = h.priced ? stock->last_price : 0;
        Totals& t = totals[h.currency];
        t.realized += h.pos.realized_profit;
        if (!h.pos.has_shares()) continue;
        t.holdings += 1;
        t.cost += h.pos.cost_basis;
        t.value += h.priced ? h.pos.market_value(h.price) : h.pos.cost_basis;
        if (stock && (oldest_price.empty() || stock->last_price_at < oldest_price)) oldest_price = stock->last_price_at;
        held.push_back(h);
    }
    if (held.empty()) {
        return {true, "You don't hold any shares right now. Everything you bought has been sold; see `/history` for "
                      "your trades and `/profit` for what they made."};
    }
    std::sort(held.begin(), held.end(), [](const Holding& a, const Holding& b) {
        if (a.currency != b.currency) return a.currency < b.currency;
        return a.pos.market_value(a.price) > b.pos.market_value(b.price);
    });

    std::ostringstream table;
    table << pad("STOCK", 9) << pad("SHARES", 13) << pad("AVG COST", 11) << pad("PRICE", 11) << pad("VALUE", 12)
          << pad("WEIGHT", 7) << "P/L\n";
    for (const auto& h : held) {
        const std::string& cur = h.currency;
        table << pad(h.symbol, 9) << pad(format_shares(h.pos.shares), 13) << pad(format_money(h.pos.average_cost(), cur), 11);
        if (!h.priced) {
            table << pad("?", 11) << pad("?", 12) << pad("?", 7) << "?\n";
            continue;
        }
        const double value = h.pos.market_value(h.price);
        const double pl = h.pos.unrealized_profit(h.price);
        const double weight = totals[cur].value > 0 ? value / totals[cur].value : 0;
        table << pad(format_money(h.price, cur), 11) << pad(format_money(value, cur), 12) << pad(format_percent(weight, 0), 7)
              << format_signed_money(pl, cur) << " ("
              << signed_percent(h.pos.cost_basis > 0 ? pl / h.pos.cost_basis : 0) << ")\n";
    }

    std::ostringstream m;
    m << "**Your portfolio**";
    if (!oldest_price.empty()) m << " (prices as of " << readable_time(oldest_price) << ")";
    m << "\n```\n" << table.str() << "```";
    for (const auto& [cur, t] : totals) {
        if (t.holdings == 0 && std::fabs(t.realized) < 0.005) continue;
        const double pl = t.value - t.cost;
        m << "\n**" << (totals.size() > 1 ? cur + ":" : std::string("Total:")) << "** " << format_money(t.value, cur)
          << " in " << t.holdings << " holding" << (t.holdings == 1 ? "" : "s") << " (paid " << format_money(t.cost, cur)
          << "), unrealized " << format_signed_money(pl, cur) << " (" << signed_percent(t.cost > 0 ? pl / t.cost : 0)
          << "), realized so far " << format_signed_money(t.realized, cur);
    }
    if (totals.size() > 1) m << "\nCurrencies are shown separately, not converted.";
    m << "\nWEIGHT = share of your holdings in that currency. `/history` lists every trade.";
    return {true, m.str()};
}

CommandReply CommandService::history(const std::string& user, const std::string& symbol_input) {
    std::string symbol;
    if (!trim(symbol_input).empty()) {
        CommandReply err;
        if (!parse_symbol(symbol_input, symbol, err)) return err;
    }
    // Replay all of the user's trades so each sale's profit matches /profit, then filter.
    std::vector<TradeEntry> entries;
    for (auto& e : trade_history(repo_.transactions(user))) {
        if (symbol.empty() || e.tx.symbol == symbol) entries.push_back(std::move(e));
    }
    if (entries.empty()) {
        return {true, symbol.empty() ? "You have no recorded trades yet. Record one with `/buy`."
                                     : "You have no recorded trades for **" + symbol + "**."};
    }

    std::map<std::string, std::string> currencies;
    auto currency_of = [&](const std::string& sym) -> const std::string& {
        auto it = currencies.find(sym);
        if (it != currencies.end()) return it->second;
        const auto stock = repo_.get_stock(sym);
        return currencies[sym] = stock ? stock->currency : "USD";
    };
    struct Totals {
        double bought = 0, sold = 0, realized = 0;
        int buys = 0, sells = 0;
    };
    std::map<std::string, Totals> totals;
    for (const auto& e : entries) {
        Totals& t = totals[currency_of(e.tx.symbol)];
        if (e.tx.side == TxSide::Buy) {
            t.bought += e.tx.total;
            t.buys += 1;
        } else {
            t.sold += e.tx.total;
            t.realized += e.realized_profit;
            t.sells += 1;
        }
    }

    std::ostringstream footer;
    for (const auto& [cur, t] : totals) {
        footer << "\n**" << (totals.size() > 1 ? cur + ":" : std::string("Total:")) << "** " << t.buys << " buy"
               << (t.buys == 1 ? "" : "s") << " for " << format_money(t.bought, cur);
        if (t.sells > 0) {
            footer << ", " << t.sells << " sale" << (t.sells == 1 ? "" : "s") << " for " << format_money(t.sold, cur)
                   << ", realized profit " << format_signed_money(t.realized, cur);
        } else {
            footer << ", no sales yet";
        }
    }

    const bool one_stock = !symbol.empty();
    std::string header = pad("DATE", 12) + pad("SIDE", 6) + (one_stock ? "" : pad("STOCK", 9)) + pad("SHARES", 13) +
                         pad("PRICE", 11) + pad("TOTAL", 12) + (one_stock ? pad("PROFIT", 12) + "HOLDING" : "PROFIT") + "\n";
    std::string title = one_stock ? "**" + symbol + " trading history** (newest first)\n```\n"
                                  : "**Your trading history** (newest first)\n```\n";
    std::string rows;
    size_t shown = 0;
    const size_t reserve = title.size() + header.size() + footer.str().size() + 200;  // closing fence and the "showing" line
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        const auto& e = *it;
        const std::string& cur = currency_of(e.tx.symbol);
        const bool sell = e.tx.side == TxSide::Sell;
        std::string row = pad(e.tx.created_at.substr(0, 10), 12) + pad(sell ? "SELL" : "BUY", 6) +
                          (one_stock ? "" : pad(e.tx.symbol, 9)) + pad(format_shares(e.shares), 13) +
                          pad(format_money(e.price, cur), 11) + pad(format_money(e.tx.total, cur), 12);
        const std::string profit = sell ? format_signed_money(e.realized_profit, cur) : "-";
        row += one_stock ? pad(profit, 12) + format_shares(e.held_after) : profit;
        row += "\n";
        if (reserve + rows.size() + row.size() > kReplyBudget) break;
        rows += row;
        ++shown;
    }

    std::ostringstream m;
    m << title << header << rows << "```";
    if (shown < entries.size()) {
        m << "Showing the latest " << shown << " of " << entries.size() << " trades.";
        if (!one_stock) m << " `/history stock:FCX` shows one stock.";
    }
    m << footer.str();
    if (totals.size() > 1) m << "\nCurrencies are shown separately, not converted.";
    return {true, m.str()};
}

CommandReply CommandService::market(const std::string& user, const std::string& filter_input) {
    (void)user;
    const int64_t now = unix_now();
    const std::string filter = to_lower(trim(filter_input));
    std::vector<MarketRow> rows;
    for (const auto& m : markets()) {
        const bool match = filter.empty() || to_lower(m.name).find(filter) != std::string::npos || to_lower(m.suffix) == filter ||
                           to_lower(m.suffix) == "." + filter;
        if (match) rows.push_back(market_row(m, now));
    }
    if (rows.empty()) return fail("No market matches **" + trim(filter_input) + "**. Type `/market` for the list.");

    std::ostringstream m;
    m << "**Markets** (times are this computer's, " << local_utc_label(now) << ")\n```\n"
      << pad("MARKET", 11) << pad("ENDING", 7) << pad("HOURS", 13) << "NOW\n";
    for (const auto& r : rows) {
        m << pad(r.market->name, 11) << pad(r.market->suffix.empty() ? "-" : r.market->suffix, 7) << pad(r.hours, 13) << r.status << "\n";
    }
    m << "```";
    if (rows.size() <= 3) {
        for (const auto& r : rows) {
            m << "\n**" << r.market->name << "**: add its stocks with the ending " << (r.market->suffix.empty() ? "(none)" : "`" + r.market->suffix + "`")
              << ", for example `/monitor stock:" << r.market->example << "`.";
        }
    } else {
        m << "ENDING goes after the ticker when you add a stock: `/monitor stock:BARC.L` (London). US tickers have none. "
             "* = lunch break. Public holidays aren't included. While a stock's market is closed its prices aren't checked.";
    }
    return {true, m.str()};
}

CommandReply CommandService::monitor(const std::string& user, const std::string& input) {
    std::string symbol;
    CommandReply err;
    if (!parse_symbol(input, symbol, err)) return err;
    if (repo_.is_watching(user, symbol)) return {true, "You are already watching **" + symbol + "**. Use `/report stock:" + symbol + "` for details."};
    std::string report, error;
    if (!ensure_stock(symbol, &report, &error)) return lookup_failed("Could not add **" + symbol + "**", symbol, error, "/monitor stock:");
    repo_.add_watch(user, symbol);
    monitor_.update_schedule();  // its market may open before the checks were due to start again
    return {true, "Now watching **" + symbol + "**. Here is the first report:\n\n" + report};
}

CommandReply CommandService::remove(const std::string& user, const std::string& input) {
    std::string symbol;
    CommandReply err;
    if (!parse_symbol(input, symbol, err)) return err;
    if (!repo_.is_watching(user, symbol)) return fail("You are not watching **" + symbol + "**.");
    const Position pos = compute_position(repo_.transactions(user, symbol));
    if (pos.has_shares()) {
        return fail("You still hold " + format_shares(pos.shares) + " shares of **" + symbol +
                    "**. You cannot remove monitoring while you have shares in it. Record the sale with `/sell` first.");
    }
    repo_.remove_watch(user, symbol);
    monitor_.update_schedule();
    return {true, "Stopped watching **" + symbol + "**."};
}

CommandReply CommandService::buy(const std::string& user, const std::string& input, const std::string& shares_text,
                                 const std::string& paid_text) {
    std::string symbol;
    CommandReply err;
    if (!parse_symbol(input, symbol, err)) return err;
    const auto shares = parse_amount(shares_text);
    const auto paid = parse_amount(paid_text);
    if (!shares || *shares <= 0) return fail("`" + shares_text + "` is not a valid number of shares.");
    if (!paid || *paid < 0) return fail("`" + paid_text + "` is not a valid amount. Example: `59.78$`.");

    std::string error;
    if (!ensure_stock(symbol, nullptr, &error)) {
        return lookup_failed("Could not record the purchase of **" + symbol + "**", symbol, error, "/buy stock:");
    }
    const bool newly_watched = repo_.add_watch(user, symbol);
    if (newly_watched) monitor_.update_schedule();

    TransactionRecord tx;
    tx.user_id = user;
    tx.symbol = symbol;
    tx.side = TxSide::Buy;
    tx.shares = *shares;
    tx.total = *paid;
    repo_.add_transaction(tx);

    const auto stock = repo_.get_stock(symbol);
    const std::string cur = stock ? stock->currency : "USD";
    const Position pos = compute_position(repo_.transactions(user, symbol));
    std::ostringstream m;
    m << "Recorded: bought " << format_shares(*shares) << " **" << symbol << "** for " << format_money(*paid, cur)
      << " (" << format_money(*paid / *shares, cur) << " per share).\n"
      << "You now hold " << format_shares(pos.shares) << " shares, average cost "
      << format_money(pos.average_cost(), cur) << " per share.";
    if (newly_watched) m << "\n" << symbol << " was added to your watch list.";
    return {true, m.str()};
}

CommandReply CommandService::sell(const std::string& user, const std::string& input, const std::string& shares_text,
                                  const std::string& received_text) {
    std::string symbol;
    CommandReply err;
    if (!parse_symbol(input, symbol, err)) return err;
    const auto shares = parse_amount(shares_text);
    const auto received = parse_amount(received_text);
    if (!shares || *shares <= 0) return fail("`" + shares_text + "` is not a valid number of shares.");
    if (!received || *received < 0) return fail("`" + received_text + "` is not a valid amount. Example: `67.87$`.");

    const Position before = compute_position(repo_.transactions(user, symbol));
    if (*shares > before.shares + kShareEpsilon) {
        return fail("You only hold " + format_shares(before.shares) + " shares of **" + symbol + "**.");
    }
    const double sold = std::min(*shares, before.shares);

    TransactionRecord tx;
    tx.user_id = user;
    tx.symbol = symbol;
    tx.side = TxSide::Sell;
    tx.shares = sold;
    tx.total = *received;
    repo_.add_transaction(tx);

    const auto stock = repo_.get_stock(symbol);
    const std::string cur = stock ? stock->currency : "USD";
    const double profit = *received - before.average_cost() * sold;
    const Position after = compute_position(repo_.transactions(user, symbol));
    std::ostringstream m;
    m << "Recorded: sold " << format_shares(sold) << " **" << symbol << "** for " << format_money(*received, cur)
      << ". Profit on this sale: **" << format_signed_money(profit, cur) << "**.\n";
    if (after.has_shares()) m << "You still hold " << format_shares(after.shares) << " shares.";
    else m << "You no longer hold any " << symbol << ". It stays on your watch list until you `/remove` it.";
    return {true, m.str()};
}

CommandReply CommandService::news(const std::string& user, const std::string& input) {
    (void)user;
    std::string symbol;
    CommandReply err;
    if (!parse_symbol(input, symbol, err)) return err;
    std::string error;
    auto report = monitor_.refresh(symbol, false, &error);  // also fetches headlines when due
    if (!report) return lookup_failed("Could not get news for **" + symbol + "**", symbol, error, "/news stock:");
    if (report->is_fund()) {
        return {true, "**" + symbol + "** is an ETF. Headlines aren't used for ETFs: a fund moves with its whole basket, "
                      "and news that names it is mostly about the fund company."};
    }
    const auto recent = repo_.recent_news(symbol, unix_to_iso(unix_now() - 14 * 86400), 8);
    return {true, format_news(symbol, recent, report->news, report->news_weight)};
}

CommandReply CommandService::suggest(const std::string& user, const std::string& side_input) {
    (void)user;
    const std::string side = to_lower(trim(side_input));
    if (!side.empty() && side != "buy" && side != "sell") return fail("Use `suggest`, `suggest buy` or `suggest sell`.");
    const bool screening = screener_ && screener_->running();
    auto results = repo_.screen_results();
    if (results.empty()) {
        if (screener_ && !screening) screener_->request_run();
        return {true, "I am checking about " + std::to_string(screener_ ? screener_->universe().size() : 70) +
                          " stocks against ten years of prices for the first time. This takes a minute or two. "
                          "Ask again shortly."};
    }

    auto edge = [&side](const ScreenResult& r) {
        return side == "buy" ? r.support_edge : side == "sell" ? r.peak_edge : r.best_edge();
    };
    std::stable_sort(results.begin(), results.end(), [&edge](const ScreenResult& a, const ScreenResult& b) { return edge(a) > edge(b); });
    std::vector<ScreenResult> picks;
    for (const auto& r : results) {
        if (edge(r) > 0 && picks.size() < 10) picks.push_back(r);
    }
    const bool none = picks.empty();
    if (none) picks.assign(results.begin(), results.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(3, results.size())));

    std::ostringstream table;
    table << pad("STOCK", 7) << pad("GOOD AT", 9) << pad("RECORD", 12) << pad("RANDOM", 8) << "NOW\n";
    for (const auto& r : picks) {
        const bool support = side == "buy" || (side.empty() && r.support_edge >= r.peak_edge);
        const int correct = support ? r.support_correct : r.peak_correct;
        const int calls = support ? r.support_signals : r.peak_signals;
        const double baseline = support ? r.baseline_support : r.baseline_peak;
        // Only flag a setup on the side the stock is proven on.
        std::string now = r.direction < 0 ? "falling" : r.direction > 0 ? "rising" : "-";
        if (support && r.direction < 0 && r.support_score >= 0.85 * r.support_cutoff) now = "falling, NEAR SUPPORT";
        if (!support && r.direction > 0 && r.peak_score >= 0.85 * r.peak_cutoff) now = "rising, NEAR PEAK";
        const std::string record = std::to_string(correct) + "/" + std::to_string(calls) + " " +
                                   format_percent(calls ? static_cast<double>(correct) / calls : 0, 0);
        table << pad(r.symbol, 7) << pad(support ? "SUPPORT" : "PEAK", 9) << pad(record, 12)
              << pad(format_percent(baseline, 0), 8) << now << "\n";
    }

    const int64_t age_h = (unix_now() - iso_to_unix(results.front().screened_at)) / 3600;
    std::ostringstream m;
    if (none) {
        m << "**No stock beats random guessing reliably right now** (out of " << results.size()
          << " screened). The closest ones:\n";
    } else {
        m << "**Most predictable stocks"
          << (side == "buy" ? " for buying at SUPPORT" : side == "sell" ? " for selling at PEAK" : "") << "**\n";
    }
    m << "```\n" << table.str() << "```\n"
      << "RECORD = calls that were right in the honest test (ten years, re-learned monthly, made blind). "
      << "RANDOM = making that call on any day. A stock is listed only if it beats RANDOM even when judged "
      << "skeptically (90% confidence).\n"
      << "Screened " << results.size() << " stocks " << (age_h < 1 ? "less than an hour" : std::to_string(age_h) + " h")
      << " ago" << (screening ? "; a fresh check is running now" : "") << ". "
      << "Picked from many stocks, some of these will be lucky streaks: follow one with `monitor SYMBOL` and watch "
      << "its live record. Not financial advice.";
    return {true, m.str()};
}

CommandReply CommandService::arena(const std::string& user, const std::string& bot) {
    (void)user;
    if (!arena_) return fail("The arena is not available here.");
    if (trim(bot).empty()) return {true, arena_->leaderboard(false)};
    for (const auto& v : arena_->view()) {
        if (to_lower(v.name) != to_lower(trim(bot))) continue;
        if (v.stage != "public" && v.stage != "benchmark") {
            return {true, "**" + v.name + "** is still " + (v.stage == "proving" ? "proving itself in private" : "in training") +
                              ". Its results become public once it has proven itself on historical and live prices."};
        }
        return {true, arena_->details(v.name)};
    }
    return {true, arena_->details(bot)};  // explains unknown names
}

CommandReply CommandService::pythia(const std::string& user) {
    (void)user;
    if (!pythia_) return fail("The AI model is not available here.");
    return {true, pythia_->status_text() + "\nNot financial advice."};
}

CommandReply CommandService::admin(const std::string& user, const std::vector<std::string>& args) {
    if (!admin_) return fail("Admin commands are not available here.");
    return admin_->handle(user, args);
}

CommandReply CommandService::run_text(const std::string& user, const std::string& text) {
    std::vector<std::string> words;
    for (const auto& w : split(trim(text), ' ')) {
        if (!trim(w).empty()) words.push_back(trim(w));
    }
    if (words.empty()) return help();
    std::string name = to_lower(words[0]);
    if (!name.empty() && (name[0] == '/' || name[0] == '!')) name.erase(0, 1);
    auto arg = [&words](size_t i) { return i < words.size() ? words[i] : std::string(); };
    auto rest = [&words](size_t from) {
        std::string s;
        for (size_t i = from; i < words.size(); ++i) s += (s.empty() ? "" : " ") + words[i];
        return s;
    };
    auto usage = [](const std::string& example) { return fail("Usage: `" + example + "`"); };

    if (name == "help") return help();
    if (name == "status") return status(user);
    if (name == "profit") return profit(user, arg(1));
    if (name == "portfolio" || name == "holdings") return portfolio(user);
    if (name == "market" || name == "markets") return market(user, rest(1));
    if (name == "history" || name == "trades") return history(user, arg(1));
    if (name == "monitor") {
        if (words.size() >= 2 && to_lower(words[1]) == "politician") {
            return words.size() < 3 ? usage("monitor politician Nancy Pelosi") : monitor_politician(user, rest(2));
        }
        return words.size() < 2 ? usage("monitor FCX") : monitor(user, arg(1));
    }
    if (name == "remove") {
        if (words.size() >= 2 && to_lower(words[1]) == "politician") {
            return words.size() < 3 ? usage("remove politician Nancy Pelosi") : remove_politician(user, rest(2));
        }
        return words.size() < 2 ? usage("remove FCX") : remove(user, arg(1));
    }
    if (name == "politicians") return politicians(user, rest(1));
    if (name == "report") return words.size() < 2 ? usage("report FCX") : report(user, arg(1));
    if (name == "suggest") return suggest(user, arg(1));
    if (name == "news") return words.size() < 2 ? usage("news FCX") : news(user, arg(1));
    if (name == "arena") return arena(user, arg(1));
    if (name == "pythia" || name == "ai") return pythia(user);
    if (name == "admin") {
        // Folder paths may contain spaces and capital letters, so pass the original words through.
        std::vector<std::string> args;
        for (const auto& w : split(trim(text), ' ')) {
            if (!trim(w).empty()) args.push_back(trim(w));
        }
        args.erase(args.begin());
        return admin(user, args);
    }
    if (name == "buy") return words.size() < 4 ? usage("buy FCX 0.858291457 59.78$") : buy(user, arg(1), arg(2), rest(3));
    if (name == "sell") return words.size() < 4 ? usage("sell FCX 0.858291457 67.87$") : sell(user, arg(1), arg(2), rest(3));
    return fail("I don't know `" + words[0] + "`. Type `help` to see what I can do.");
}

CommandReply CommandService::monitor_politician(const std::string& user, const std::string& input) {
    const std::string name = trim(input);
    if (name.empty()) return fail("Usage: `monitor politician Nancy Pelosi`.");
    if (repo_.add_politician_watch(user, to_lower(name))) {
        return {true, "Now watching **" + name + "**. You'll get a direct message when a new trade is disclosed (every word "
                      "you typed must be in the name, so a last name is enough). Every new disclosure is also posted in "
                      "the server's public channel, if an admin has set one up."};
    }
    return {true, "You're already watching **" + name + "**."};
}

CommandReply CommandService::remove_politician(const std::string& user, const std::string& input) {
    const std::string name = trim(input);
    if (name.empty()) return fail("Usage: `remove politician Nancy Pelosi`.");
    if (repo_.remove_politician_watch(user, to_lower(name))) return {true, "Stopped watching **" + name + "**."};
    return fail("You are not watching **" + name + "**.");
}

CommandReply CommandService::politicians(const std::string& user, const std::string& name_filter) {
    (void)user;
    const std::string filter = trim(name_filter);
    const auto trades = repo_.recent_politician_trades(filter, 10);
    if (trades.empty()) {
        if (!filter.empty()) return {true, "No disclosed trades found yet for **" + filter + "**."};
        return {true, "No politician trades tracked yet. The disclosure check runs once a day; try again soon, or "
                      "ask an admin to run `politicians run` in the server console."};
    }
    std::ostringstream m;
    m << "**Recent politician stock trades" << (filter.empty() ? "" : " - " + filter) << "** (newest filings first)\n";
    for (const auto& t : trades) {
        const bool has_ticker = !t.ticker.empty() && t.ticker != "--";
        m << "- **" << t.full_name() << "**";
        if (!t.office.empty()) m << " (" << t.office << ")";
        m << ": " << (t.transaction_type.empty() ? "trade" : t.transaction_type) << " **"
          << (has_ticker ? t.ticker : short_company_name(t.asset_name, 40)) << "**, " << (t.owner.empty() ? "Self" : t.owner)
          << ", " << format_whole_money(t.amount_min) << (t.amount_max ? " - " + format_whole_money(*t.amount_max) : "+");
        if (!t.transaction_date.empty()) m << ", traded " << t.transaction_date;
        if (!t.filed_date.empty()) m << ", filed " << t.filed_date;
        m << "\n";
    }
    m << "\nWatch one personally with `monitor politician <name>`. Disclosures can legally appear up to ~45 days after the "
         "trade. House filings come from the House Clerk; the Senate's site refuses connections from many networks, "
         "including outside the US. Scanned paper filings can't be read.";
    return {true, m.str()};
}

CommandReply CommandService::report(const std::string& user, const std::string& input) {
    (void)user;
    std::string symbol;
    CommandReply err;
    if (!parse_symbol(input, symbol, err)) return err;
    std::string text, error;
    if (!ensure_stock(symbol, &text, &error)) return lookup_failed("Could not analyse **" + symbol + "**", symbol, error, "/report stock:");
    return {true, text};
}

}  // namespace strojanka
