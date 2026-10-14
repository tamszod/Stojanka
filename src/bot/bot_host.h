#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

#include "common/config.h"
#include "common/instance_lock.h"
#include "core/admin_service.h"
#include "core/arena/arena_service.h"
#include "core/commands.h"
#include "core/database.h"
#include "core/market/yahoo_provider.h"
#include "core/monitor_service.h"
#include "core/news/news_provider.h"
#include "core/repository.h"
#include "core/screener.h"
#include "core/server_console.h"

namespace strojanka {

class DiscordBot;

// Owns one running bot: settings, single-instance lock, database, services and the Discord
// connection. Used by strojanka.exe (headless) and StrojankaServer (window). Not thread-safe:
// start/stop are called from one thread; the accessors are valid while running().
class BotHost {
public:
    explicit BotHost(std::string config_path);
    ~BotHost();

    // Loads the settings, checks the token, locks and upgrades the database, starts the services
    // and connects to Discord in the background. Returns false with a readable error.
    bool start(std::string* error);
    void stop();
    bool running() const { return bot_ != nullptr; }
    void wait();  // blocks until stop() is called from another thread

    bool online() const;
    std::string bot_name() const;
    const std::string& config_path() const { return config_path_; }
    const Config& config() const { return config_; }
    const std::string& started_at() const { return started_at_; }

    Database& database() { return *db_; }
    Repository& repository() { return *repo_; }
    MonitorService& monitor() { return *monitor_; }
    Screener& screener() { return *screener_; }
    AdminService& admin() { return *admin_; }
    ServerConsole& console() { return *console_; }
    arena::ArenaService& arena() { return *arena_; }

private:
    std::string config_path_;
    Config config_;
    std::string started_at_;
    std::unique_ptr<InstanceLock> lock_;
    std::unique_ptr<Database> db_;
    std::unique_ptr<Repository> repo_;
    std::unique_ptr<YahooProvider> prices_;
    std::unique_ptr<news::RssNewsProvider> news_;
    std::unique_ptr<MonitorService> monitor_;
    std::unique_ptr<Screener> screener_;
    std::unique_ptr<CommandService> commands_;
    std::unique_ptr<AdminService> admin_;
    std::unique_ptr<ml::Pythia> pythia_;
    std::unique_ptr<arena::ArenaService> arena_;
    std::unique_ptr<ServerConsole> console_;
    std::unique_ptr<DiscordBot> bot_;
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    bool stop_requested_ = false;
};

// GET /users/@me with the token: 401 = rejected, 0 = Discord unreachable, else the HTTP status.
long check_discord_token(const std::string& token);

}  // namespace strojanka
