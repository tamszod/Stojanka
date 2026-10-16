#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "common/config.h"
#include "core/command_reply.h"
#include "core/database.h"
#include "core/monitor_service.h"
#include "core/repository.h"

namespace strojanka {

// app_meta key holding the Discord channel where public bot/politician-trade reports go. Kept
// as "arena_channel_id" in storage since that's what existing installs already have saved;
// it now means "the public announcements channel" more broadly, not just the arena.
inline constexpr const char* kPublicChannelKey = "arena_channel_id";

inline constexpr const char* kBackupExtension = ".strojanka_backup";
inline constexpr int64_t kBackupReminderAfterSeconds = 21 * 86400;  // 3 weeks without a backup
inline constexpr int64_t kBackupReminderRepeatSeconds = 2 * 86400;  // then every 2 days until there is one

struct BackupResult {
    bool ok = false;
    std::string path;
    uint64_t bytes = 0;
    std::string error;
};

// The `admin` command and the bot's own health checks. Admins are the owner(s) of the Discord
// application (detected automatically) plus anyone they add. Admins receive warnings such as
// low disk space, and can move the database to another folder or drive while the bot runs.
class AdminService {
public:
    AdminService(Database& db, Repository& repo, MonitorService& monitor, const Config& config, std::string config_path);

    // Owner(s) of the Discord application; they are always admins and cannot be removed.
    void set_owners(const std::vector<std::string>& ids);
    bool is_admin(const std::string& user);
    std::vector<std::string> admins();  // owners first, no duplicates

    // `admin`, `admin add <id>`, `admin remove <id>`, `admin list`, `admin movedb <folder>`.
    CommandReply handle(const std::string& user, const std::vector<std::string>& args);
    // Same commands for the local server console, which is trusted (whoever sits at the server).
    CommandReply handle_console(const std::vector<std::string>& args);
    std::string status_text();

    // Moves the database into `folder` (keeping its file name) and updates strojanka.conf.
    CommandReply move_database(const std::string& folder);

    // Saves a checked copy of the database as <backup_folder>/strojanka-<time>.strojanka_backup.
    BackupResult create_backup();
    std::string backup_folder() const;   // absolute
    std::string last_backup_at();        // ISO UTC, "" = never

    // Hourly check: low disk space on the database drive, price data unreachable, no backup for
    // 3 weeks. Sends each warning to every admin through `notifier`, at most once per day (outage:
    // every 6 hours; backup: every 2 days, also by email when backup_email is set).
    void check_health(Notifier& notifier);

    struct Disk {
        bool known = false;
        uint64_t free = 0;
        uint64_t capacity = 0;
    };
    Disk disk_space();

private:
    CommandReply run(const std::string& user, const std::vector<std::string>& args);
    CommandReply set_email(const std::string& address);
    void warn_admins(Notifier& notifier, const std::string& message);
    void remind_backup(Notifier& notifier);

    Database& db_;
    Repository& repo_;
    MonitorService& monitor_;
    Config config_;
    std::string config_path_;
    std::mutex owners_mutex_;
    std::vector<std::string> owners_;
};

// "<@123>" / "<@!123>" / "123" -> "123"; "" if it is not a Discord user ID.
std::string parse_user_id(const std::string& text);
std::string format_bytes(uint64_t bytes);

}  // namespace strojanka
