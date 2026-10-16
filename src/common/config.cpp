#include "common/config.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "common/strings.h"
#include "common/utf8_path.h"

namespace fs = std::filesystem;

namespace strojanka {

static int to_int(const std::string& v, int fallback) {
    try {
        return std::stoi(v);
    } catch (...) {
        return fallback;
    }
}

std::optional<Config> load_config(const std::string& path, std::string* error) {
    std::ifstream in(utf8_path(path));
    if (!in) {
        if (error) *error = "cannot open config file: " + path;
        return std::nullopt;
    }
    Config c;
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            if (error) *error = "line " + std::to_string(line_no) + ": expected key=value";
            return std::nullopt;
        }
        const std::string key = to_lower(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "discord_token") c.discord_token = value;
        else if (key == "guild_id") c.guild_id = value;
        else if (key == "database_path") c.database_path = value;
        else if (key == "poll_interval_minutes") c.poll_interval_minutes = to_int(value, c.poll_interval_minutes);
        else if (key == "max_parallel_fetches") c.max_parallel_fetches = to_int(value, c.max_parallel_fetches);
        else if (key == "log_file") c.log_file = value;
        else if (key == "log_level") c.log_level = value;
        else if (key == "installed_version") c.installed_version = value;
        else if (key == "screener_symbols") c.screener_symbols = value;
        else if (key == "low_disk_warning_mb") c.low_disk_warning_mb = to_int(value, c.low_disk_warning_mb);
        else if (key == "backup_folder") c.backup_folder = value;
        else if (key == "backup_email") c.backup_email = value;
        else if (key == "smtp_server") c.smtp_server = value;
        else if (key == "smtp_user") c.smtp_user = value;
        else if (key == "smtp_password") c.smtp_password = value;
        // Unknown keys are ignored so older builds can read configs written by newer ones.
    }
    if (c.poll_interval_minutes < 1) c.poll_interval_minutes = 1;
    if (c.max_parallel_fetches < 1) c.max_parallel_fetches = 1;
    return c;
}

bool save_config(const Config& c, const std::string& path, std::string* error) {
    std::ostringstream out;
    out << "# Strojanka configuration. Lines starting with # are comments.\n"
        << "# Keep this file private: it contains your Discord bot token.\n\n"
        << "discord_token=" << c.discord_token << "\n"
        << "# Legacy: if set, slash commands left on this server by older versions are removed.\n"
        << "guild_id=" << c.guild_id << "\n"
        << "# Relative paths are resolved against this file's folder.\n"
        << "database_path=" << c.database_path << "\n"
        << "poll_interval_minutes=" << c.poll_interval_minutes << "\n"
        << "max_parallel_fetches=" << c.max_parallel_fetches << "\n"
        << "log_file=" << c.log_file << "\n"
        << "# debug, info, warn or error\n"
        << "log_level=" << c.log_level << "\n"
        << "installed_version=" << c.installed_version << "\n"
        << "# Stocks the `suggest` command screens, comma-separated. Empty = about 70 large US stocks.\n"
        << "screener_symbols=" << c.screener_symbols << "\n"
        << "# Admins get a Discord warning when the database drive has less free space than this (MB).\n"
        << "low_disk_warning_mb=" << c.low_disk_warning_mb << "\n"
        << "# Where the Backup button saves .strojanka_backup files. Best on another drive than the database.\n"
        << "backup_folder=" << c.backup_folder << "\n"
        << "# Email for backup reminders (3 weeks without a backup, then every 2 days). Empty = Discord only.\n"
        << "backup_email=" << c.backup_email << "\n"
        << "# Mail server that sends it, e.g. smtps://smtp.gmail.com:465. Empty = chosen from the address.\n"
        << "smtp_server=" << c.smtp_server << "\n"
        << "# Login for that server (empty = the address above) and its password; Gmail needs an app password.\n"
        << "smtp_user=" << c.smtp_user << "\n"
        << "smtp_password=" << c.smtp_password << "\n";

    std::error_code ec;
    const fs::path p = utf8_path(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (error) *error = "cannot write config file: " + path;
        return false;
    }
    f << out.str();
    f.close();
#ifndef _WIN32
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
#endif
    return true;
}

std::string resolve_config_path(const std::string& config_file, const std::string& path) {
    const fs::path p = utf8_path(path);
    if (p.is_absolute() || path == ":memory:") return path;
    const fs::path base = fs::absolute(utf8_path(config_file)).parent_path();
    return path_utf8((base / p).lexically_normal());
}

}  // namespace strojanka
