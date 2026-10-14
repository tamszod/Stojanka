#pragma once

#include <string>
#include <vector>

#include "core/admin_service.h"
#include "core/arena/arena_service.h"
#include "core/command_reply.h"
#include "core/commands.h"
#include "core/monitor_service.h"
#include "core/politicians/politician_service.h"
#include "core/repository.h"
#include "core/screener.h"

namespace strojanka {

// One row of the server window's "watched stocks" table.
struct WatchedStockRow {
    std::string symbol, name, currency, trend, category;
    double price = 0;
    int watchers = 0;
    std::string last_signal;       // "PEAK 2026-10-09 (CORRECT)" or ""
};

// The command line of the server window. Whoever sits at the server is trusted, so admin
// commands work without being a Discord admin. Replies use the same light Markdown as Discord.
class ServerConsole {
public:
    ServerConsole(Repository& repo, MonitorService& monitor, Screener& screener, AdminService& admin, CommandService& commands)
        : repo_(repo), monitor_(monitor), screener_(screener), admin_(admin), commands_(commands) {}

    CommandReply run(const std::string& line);
    void set_arena(arena::ArenaService* arena) { arena_ = arena; }
    void set_politicians(politicians::PoliticianService* politicians) { politicians_ = politicians; }
    std::vector<WatchedStockRow> watched_stocks();

private:
    std::string help() const;
    std::string users();
    std::string stocks();

    Repository& repo_;
    MonitorService& monitor_;
    Screener& screener_;
    AdminService& admin_;
    CommandService& commands_;
    arena::ArenaService* arena_ = nullptr;
    politicians::PoliticianService* politicians_ = nullptr;
};

}  // namespace strojanka
