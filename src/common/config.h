#pragma once

#include <optional>
#include <string>

namespace strojanka {

// Runtime configuration, stored as a plain `key=value` file (strojanka.conf) next to the
// executable. Relative paths inside it are resolved against the config file's folder.
struct Config {
    std::string discord_token;
    std::string guild_id;  // optional: registers slash commands on one server instantly
    std::string database_path = "data/strojanka.db";
    int poll_interval_minutes = 15;
    int max_parallel_fetches = 4;
    std::string log_file = "strojanka.log";
    std::string log_level = "info";
    std::string installed_version;
    std::string screener_symbols;  // comma-separated stocks for `suggest`; empty = built-in list
    int low_disk_warning_mb = 1024; // admins are warned when the database drive has less free space
    std::string backup_folder = "backups";  // where Backup writes .strojanka_backup files (relative = next to the config)
    // Backup reminders by email (every 3 weeks without a backup, then every 2 days). Empty = no email.
    std::string backup_email;
    std::string smtp_server;    // e.g. "smtps://smtp.gmail.com:465"; empty = guessed from the address
    std::string smtp_user;      // empty = backup_email
    std::string smtp_password;  // for Gmail: an app password
};

std::optional<Config> load_config(const std::string& path, std::string* error);
bool save_config(const Config& config, const std::string& path, std::string* error);

// Resolves `path` relative to the folder that contains `config_file` (absolute paths pass through).
std::string resolve_config_path(const std::string& config_file, const std::string& path);

}  // namespace strojanka
