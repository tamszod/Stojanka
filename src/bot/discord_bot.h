#pragma once

#include <dpp/dpp.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "common/config.h"
#include "core/admin_service.h"
#include "core/arena/arena_service.h"
#include "core/commands.h"
#include "core/monitor_service.h"
#include "core/repository.h"
#include "core/screener.h"
#include "core/thread_pool.h"

namespace strojanka {

// Discord front-end: maps slash commands to CommandService and delivers alerts as DMs.
class DiscordBot : public Notifier {
public:
    DiscordBot(const Config& config, Repository& repo, MonitorService& monitor, CommandService& commands, Screener& screener,
               AdminService& admin, arena::ArenaService& arena);
    ~DiscordBot() override;

    void run();    // connects and blocks until the process is stopped
    void start();  // connects in the background and returns immediately
    bool online() const { return online_; }
    std::string name();

    void send(int64_t notification_id, const std::string& user_id, const std::string& message) override;
    void send_direct(const std::string& user_id, const std::string& message) override;

private:
    void register_commands();
    void load_owners();  // application owner(s) become admins, then the first health check runs
    void post_to_arena_channel(const std::string& message);
    void on_command(const dpp::slashcommand_t& event);
    void on_message(const dpp::message_create_t& event);
    CommandReply dispatch(const std::string& name, const std::string& user, const std::string& stock,
                          const std::string& shares, const std::string& amount, const std::vector<std::string>& admin_args);

    Config config_;
    Repository& repo_;
    MonitorService& monitor_;
    CommandService& commands_;
    Screener& screener_;
    AdminService& admin_;
    arena::ArenaService& arena_;
    std::atomic<bool> online_{false};
    bool started_ = false;  // per instance (dpp::run_once is per process, which breaks Stop/Start)
    std::mutex name_mutex_;
    std::string name_;
    dpp::cluster cluster_;
    ThreadPool workers_;
};

}  // namespace strojanka
