#include "core/commands.h"

#include <algorithm>
#include <map>
#include <sstream>

#include "common/strings.h"
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

std::string pad(const std::string& s, size_t width) { return s.size() >= width ? s : s + std::string(width - s.size(), ' '); }

}  // namespace

CommandReply CommandService::help() const {
    std::ostringstream m;
    m << "**Strojanka " << STROJANKA_VERSION << "** watches your stocks and tells you when they reach a PEAK "
      << "(time to sell) or a SUPPORT level (time to buy).\n\n"
      << "`/help` - this message\n"
      << "`/status` - your watched stocks with price, trend and how much money you have in them\n"
      << "`/profit` - profit for every stock you traded; `/profit stock:FCX` for one stock\n"
      << "`/monitor stock:FCX` - start watching a stock or ETF (you get an analysis report). ETFs work the same: "
      << "`SPY`, or a European listing with its exchange ending such as `CSPX.L` (London) or `SXR8.DE` (Xetra)\n"
      << "`/remove stock:FCX` - stop watching a stock (only when you hold no shares of it)\n"
      << "`/buy stock:FCX shares:0.858291457 paid:59.78$` - record a purchase; `paid` is the total you paid\n"
      << "`/sell stock:FCX shares:0.858291457 received:67.87$` - record a sale; `received` is the total you got\n"
      << "`/report stock:FCX` - fresh analysis: category, support/resistance, learned accuracy\n"
      << "`/suggest` - stocks whose signals have proven reliable; `/suggest side:buy` or `side:sell` for one kind\n"
      << "`/news stock:FCX` - recent headlines, their tone, and how news affects the signals\n"
      << "`/arena` - the paper-trading bots ($1,000 each) and how they are doing; `/arena bot:Athena` for one bot\n"
      << "`/pythia` - the built-in AI model: how well it did on years it never saw, and today's strongest candidates\n"
      << "`/politicians` - recent US Senate stock trades; `monitor politician:Pelosi` to watch one personally (DM on a new trade)\n"
      << "`/admin` - admin panel: disk space, database location, admins (admins only)\n\n"
      << "In a direct message with me you can also just type the command, for example "
      << "`buy FCX 0.858291457 59.78$`, `sell FCX 0.858291457 67.87$`, `profit FCX` or `status`.\n\n"
      << "Alerts arrive as direct messages. PEAK alerts are only sent for stocks you hold.\n"
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
    std::vector<SymbolMatch> matches;
    for (auto& m : monitor_.search_symbols(symbol)) {
        if (m.symbol != symbol) matches.push_back(std::move(m));
        if (matches.size() == 5) break;
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
    table << pad("STOCK", 9) << pad("PRICE", 12) << pad("TREND", 12) << pad("SHARES", 14) << pad("VALUE", 12) << "P/L\n";
    for (const auto& sym : symbols) {
        const auto stock = repo_.get_stock(sym);
        const Position pos = compute_position(repo_.transactions(user, sym));
        const double price = stock ? stock->last_price : 0;
        const std::string cur = stock ? stock->currency : "USD";
        currency = cur;
        table << pad(sym, 9) << pad(stock ? format_money(price, cur) : "?", 12) << pad(stock ? stock->trend : "?", 12);
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

CommandReply CommandService::monitor(const std::string& user, const std::string& input) {
    std::string symbol;
    CommandReply err;
    if (!parse_symbol(input, symbol, err)) return err;
    if (repo_.is_watching(user, symbol)) return {true, "You are already watching **" + symbol + "**. Use `/report stock:" + symbol + "` for details."};
    std::string report, error;
    if (!ensure_stock(symbol, &report, &error)) return lookup_failed("Could not add **" + symbol + "**", symbol, error, "/monitor stock:");
    repo_.add_watch(user, symbol);
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
        return {true, "Now watching **" + name + "**. You'll get a direct message when a new trade is disclosed. Every new "
                      "Senate disclosure is also posted in the server's public channel, if an admin has set one up."};
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
        return {true, "No politician trades tracked yet. The Senate disclosure check runs once a day; try again soon, or "
                      "ask an admin to run `politicians run` in the server console."};
    }
    std::ostringstream m;
    m << "**Recent politician stock trades" << (filter.empty() ? "" : " - " + filter) << "** (US Senate only)\n";
    for (const auto& t : trades) {
        const bool has_ticker = !t.ticker.empty() && t.ticker != "--";
        m << "- **" << t.full_name() << "**";
        if (!t.office.empty()) m << " (" << t.office << ")";
        m << ": " << (t.transaction_type.empty() ? "trade" : t.transaction_type) << " " << (has_ticker ? t.ticker : t.asset_name)
          << ", " << format_money(t.amount_min) << (t.amount_max ? "-" + format_money(*t.amount_max) : "+");
        if (!t.transaction_date.empty()) m << ", " << t.transaction_date;
        m << "\n";
    }
    m << "\nWatch one personally with `monitor politician <name>`. Disclosures can legally appear up to ~45 days after the "
         "trade. Senate only for now: the House only publishes filings as PDFs, which isn't supported yet.";
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
