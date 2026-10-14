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
    CommandReply monitor(const std::string& user, const std::string& symbol);
    CommandReply remove(const std::string& user, const std::string& symbol);
    CommandReply buy(const std::string& user, const std::string& symbol, const std::string& shares, const std::string& paid);
    CommandReply sell(const std::string& user, const std::string& symbol, const std::string& shares, const std::string& received);
    CommandReply report(const std::string& user, const std::string& symbol);
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

    Repository& repo_;
    MonitorService& monitor_;
    Screener* screener_;
    AdminService* admin_ = nullptr;
    arena::ArenaService* arena_ = nullptr;
    ml::Pythia* pythia_ = nullptr;
};

}  // namespace strojanka
