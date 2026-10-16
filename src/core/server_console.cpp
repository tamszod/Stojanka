#include "core/server_console.h"

#include <sstream>

#include "common/strings.h"
#include "core/log.h"
#include "core/portfolio.h"
#include "core/version.h"

namespace strojanka {

namespace {

std::vector<std::string> words_of(const std::string& line) {
    std::vector<std::string> out;
    for (const auto& w : split(trim(line), ' ')) {
        if (!trim(w).empty()) out.push_back(trim(w));
    }
    return out;
}

std::string pad(const std::string& s, size_t width) {
    const size_t chars = utf8_length(s);  // € and £ are several bytes
    return chars >= width ? s + " " : s + std::string(width - chars, ' ');
}

}  // namespace

std::string ServerConsole::help() const {
    return "**Server commands**\n"
           "`status` - server status (database, disk, users, last price check, admins)\n"
           "`stocks` - every watched stock with price, trend and watchers\n"
           "`users` - Discord users with how many stocks they watch and trade\n"
           "`check` - run a price check for all watched stocks now\n"
           "`screen` - re-run the `suggest` screener now (takes about 15 s)\n"
           "`report FCX` | `news FCX` | `suggest [buy|sell]` | `pythia` - same as in Discord\n"
           "`as <user ID> <command>` - run a Discord command as that user, e.g. `as 3550... status`\n"
           "`arena` - all trading bots, including private ones | `arena Athena` - one bot in detail\n"
           "`arena run` - trade and re-judge now | `arena channel <channel ID>` - where public bots report\n"
           "`politicians` - recent US Senate stock trades | `politicians Pelosi` - one politician's trades\n"
           "`politicians run` - check for new Senate filings now | `politicians channel <channel ID>` - same public channel\n"
           "`admin add <user ID>` | `admin remove <user ID>` | `admin movedb <folder>` - admin commands\n"
           "`loglevel debug|info|warn|error` - how much the log shows\n"
           "`version` | `clear` | `help`";
}

std::string ServerConsole::users() {
    std::ostringstream m;
    m << "**Users**\n```\n" << pad("DISCORD ID", 21) << pad("WATCHING", 10) << pad("HOLDING", 9) << "ADMIN\n";
    const auto ids = repo_.user_ids();  // everyone who ever ran a command
    for (const auto& id : ids) {
        int holding = 0;
        for (const auto& symbol : repo_.traded_symbols(id)) holding += compute_position(repo_.transactions(id, symbol)).has_shares();
        m << pad(id, 21) << pad(std::to_string(repo_.user_watchlist(id).size()), 10) << pad(std::to_string(holding), 9)
          << (admin_.is_admin(id) ? "yes" : "") << "\n";
    }
    m << "```" << ids.size() << " user(s).";
    return m.str();
}

std::vector<WatchedStockRow> ServerConsole::watched_stocks() {
    std::vector<WatchedStockRow> rows;
    for (const auto& symbol : repo_.watched_symbols()) {
        WatchedStockRow r;
        r.symbol = symbol;
        r.watchers = static_cast<int>(repo_.watchers(symbol).size());
        if (auto s = repo_.get_stock(symbol)) {
            r.name = s->name;
            r.currency = s->currency;
            r.price = s->last_price;
            r.trend = s->trend;
            r.category = s->category;
        }
        if (auto sig = repo_.latest_signal(symbol)) {
            r.last_signal = std::string(to_string(sig->side)) + " " + sig->bar_date + " (" + to_string(sig->outcome) + ")";
        }
        rows.push_back(r);
    }
    return rows;
}

std::string ServerConsole::stocks() {
    const auto rows = watched_stocks();
    if (rows.empty()) return "No stocks are being watched yet.";
    std::ostringstream m;
    m << "**Watched stocks**\n```\n" << pad("STOCK", 9) << pad("COMPANY", 19) << pad("PRICE", 12) << pad("TREND", 12)
      << pad("WATCHERS", 10) << "LAST ALERT\n";
    for (const auto& r : rows) {
        m << pad(r.symbol, 9) << pad(r.name.empty() ? "-" : short_company_name(r.name, 18), 19)
          << pad(r.price > 0 ? format_money(r.price, r.currency) : "?", 12) << pad(r.trend, 12)
          << pad(std::to_string(r.watchers), 10) << (r.last_signal.empty() ? "-" : r.last_signal) << "\n";
    }
    m << "```";
    return m.str();
}

CommandReply ServerConsole::run(const std::string& line) {
    const auto words = words_of(line);
    if (words.empty()) return {true, ""};
    const std::string cmd = to_lower(words[0]);
    auto arg = [&words](size_t i) { return i < words.size() ? words[i] : std::string(); };
    std::vector<std::string> rest(words.begin() + 1, words.end());

    if (cmd == "help" || cmd == "?") return {true, help()};
    if (cmd == "version") return {true, std::string("Strojanka ") + STROJANKA_VERSION};
    if (cmd == "status") return {true, admin_.status_text()};
    if (cmd == "stocks") return {true, stocks()};
    if (cmd == "users") return {true, users()};
    if (cmd == "admin") return admin_.handle_console(rest);
    if (cmd == "check") {
        monitor_.run_cycle();
        const CycleStats c = monitor_.last_cycle();
        return {c.total == 0 || c.refreshed > 0,
                c.total == 0 ? "No stocks are being watched, nothing to check."
                             : "Price check done: " + std::to_string(c.refreshed) + "/" + std::to_string(c.total) +
                                   " stocks refreshed." + (c.refreshed < c.total ? " Last error: " + c.last_error : "")};
    }
    if (cmd == "screen") {
        if (screener_.running()) return {false, "The screener is already running."};
        const size_t n = screener_.run_now();
        return {n > 0, "Screened " + std::to_string(n) + " stocks. Type `suggest` to see the result."};
    }
    if (cmd == "loglevel") {
        const std::string level = to_lower(arg(1));
        if (level != "debug" && level != "info" && level != "warn" && level != "error") {
            return {false, "Usage: `loglevel debug|info|warn|error`"};
        }
        log_set_level(parse_log_level(level));
        return {true, "Log level set to " + level + " (until the server restarts)."};
    }
    if (cmd == "as") {
        const std::string user = parse_user_id(arg(1));
        if (user.empty() || words.size() < 3) return {false, "Usage: `as <Discord user ID> <command>`, e.g. `as 355014703254339595 status`"};
        std::string command;
        for (size_t i = 2; i < words.size(); ++i) command += (i > 2 ? " " : "") + words[i];
        repo_.ensure_user(user, "");
        return commands_.run_text(user, command);
    }
    if (cmd == "report" || cmd == "news" || cmd == "suggest" || cmd == "pythia" || cmd == "ai") {
        return commands_.run_text("console", line);
    }
    if (cmd == "arena") {
        if (!arena_) return {false, "The arena is not running."};
        const std::string sub = to_lower(arg(1));
        if (sub.empty()) return {true, arena_->leaderboard(true)};
        if (sub == "run") {
            if (arena_->busy()) return {false, "The arena is already working."};
            arena_->tick(arena::ArenaService::last_complete_date(unix_now()));
            return {true, arena_->leaderboard(true)};
        }
        if (sub == "channel") {
            std::vector<std::string> args = {"channel"};
            if (words.size() > 2) args.push_back(words[2]);
            return admin_.handle_console(args);
        }
        return {true, arena_->details(arg(1))};
    }
    if (cmd == "politicians") {
        const std::string politicians_sub = to_lower(arg(1));
        if (politicians_sub == "channel") {  // reuses the same shared public channel as the arena
            std::vector<std::string> args = {"channel"};
            if (words.size() > 2) args.push_back(words[2]);
            return admin_.handle_console(args);
        }
        if (!politicians_) return {false, "Politician tracking is not running."};
        if (politicians_sub == "run") {
            if (politicians_->busy()) return {false, "A politician trade check is already running."};
            const size_t n = politicians_->tick();
            return {true, "Checked for new Senate filings: " + std::to_string(n) + " new trade(s). Type `politicians` to see them."};
        }
        return commands_.run_text("console", line);
    }
    return {false, "Unknown command `" + words[0] + "`. Type `help`."};
}

}  // namespace strojanka
