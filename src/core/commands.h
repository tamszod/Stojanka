#pragma once

#include <string>

#include "core/command_reply.h"
#include "core/monitor_service.h"
#include "core/repository.h"

namespace strojanka {

// Implements every user command. Pure business logic: no Discord types, so the same
// commands can later be exposed through other front-ends (web, Telegram, CLI...).
class Screener;
class AdminService;
namespace arena {
class ArenaService;
}
namespace ml {
class Pythia;
}

class CommandService {
public:
    CommandService(Repository& repo, MonitorService& monitor, Screener* screener = nullptr)
        : repo_(repo), monitor_(monitor), screener_(screener) {}

    CommandReply help() const;
    CommandReply status(const std::string& user);
    CommandReply profit(const std::string& user, const std::string& symbol_or_empty);
    // What the user holds now: shares, average cost, price, value, weight and profit, per currency.
    CommandReply portfolio(const std::string& user);
    // Recorded buys and sells, newest first, with the profit each sale realized. symbol: "" = all.
    CommandReply history(const std::string& user, const std::string& symbol_or_empty);
    CommandReply monitor(const std::string& user, const std::string& symbol);
    CommandReply remove(const std::string& user, const std::string& symbol);
    // Personal politician-trade watch (separate from stock watching): a direct message when
    // this politician discloses a new trade. Every new disclosure is also posted to the
    // server's public channel regardless of who is personally watching (see admin_service.h).
    CommandReply monitor_politician(const std::string& user, const std::string& name);
    CommandReply remove_politician(const std::string& user, const std::string& name);
    // Recent disclosed trades. name_filter: "" for everyone, otherwise a substring of the name.
    CommandReply politicians(const std::string& user, const std::string& name_filter);
    CommandReply buy(const std::string& user, const std::string& symbol, const std::string& shares, const std::string& paid);
    CommandReply sell(const std::string& user, const std::string& symbol, const std::string& shares, const std::string& received);
    CommandReply report(const std::string& user, const std::string& symbol);
    // Stock exchanges: ticker ending, hours (this computer's time) and whether they're open. filter: "" = all.
    CommandReply market(const std::string& user, const std::string& filter);
    // Recent headlines for a stock with their tone and how news affects its signals.
    CommandReply news(const std::string& user, const std::string& symbol);
    // Stocks whose calls proved reliable in the honest test. side: "" (any), "buy" or "sell".
    CommandReply suggest(const std::string& user, const std::string& side);

    // `admin ...` (admins only). Without an AdminService the command is unavailable.
    void set_admin(AdminService* admin) { admin_ = admin; }

    // `arena` / `arena <bot>`: the paper-trading bots. Users only see bots that went public.
    void set_arena(arena::ArenaService* arena) { arena_ = arena; }
    CommandReply arena(const std::string& user, const std::string& bot);

    // `pythia`: the AI model's tested track record and today's strongest candidates.
    void set_pythia(ml::Pythia* pythia) { pythia_ = pythia; }
    CommandReply pythia(const std::string& user);
    CommandReply admin(const std::string& user, const std::vector<std::string>& args);

    // Plain-text form used in direct messages, e.g. "buy FCX 0.858291457 59.78$".
    // A leading "/" or "!" is ignored, command names are case-insensitive.
    CommandReply run_text(const std::string& user, const std::string& text);

private:
    // Makes sure the stock exists in the database (downloading history the first time).
    bool ensure_stock(const std::string& symbol, std::string* report_text, std::string* error);
    // "<intro>: <error>", plus matching listings when the ticker wasn't found (CSPX -> CSPX.L).
    // example_command is the slash command to show with the first match, e.g. "/monitor stock:".
    CommandReply lookup_failed(const std::string& intro, const std::string& symbol, const std::string& error,
                               const std::string& example_command);

    Repository& repo_;
    MonitorService& monitor_;
    Screener* screener_;
    AdminService* admin_ = nullptr;
    arena::ArenaService* arena_ = nullptr;
    ml::Pythia* pythia_ = nullptr;
};

}  // namespace strojanka
