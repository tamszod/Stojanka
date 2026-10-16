#include "core/admin_service.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

#include "common/strings.h"
#include "core/arena/arena_service.h"
#include "common/utf8_path.h"
#include "core/email.h"
#include "core/log.h"
#include "core/markets.h"
#include "core/version.h"

namespace fs = std::filesystem;

namespace strojanka {

namespace {

constexpr int64_t kDiskWarningRepeatSeconds = 24 * 3600;
constexpr int64_t kOutageWarningRepeatSeconds = 6 * 3600;
constexpr const char* kDiskWarnedKey = "disk_warned_at";
constexpr const char* kOutageWarnedKey = "outage_warned_at";
constexpr const char* kLastBackupKey = "last_backup_at";
constexpr const char* kLastBackupPathKey = "last_backup_path";
constexpr const char* kBackupSinceKey = "backup_watch_since";  // when counting started on a database never backed up
constexpr const char* kBackupRemindedKey = "backup_reminded_at";

bool due(const std::string& last_iso, int64_t every) { return last_iso.empty() || unix_now() - iso_to_unix(last_iso) >= every; }

std::string ago(const std::string& iso) {
    if (iso.empty()) return "never";
    const int64_t s = unix_now() - iso_to_unix(iso);
    if (s < 120) return "just now";
    if (s < 7200) return std::to_string(s / 60) + " min ago";
    if (s < 2 * 86400) return std::to_string(s / 3600) + " h ago";
    return std::to_string(s / 86400) + " days ago";
}

}  // namespace

std::string parse_user_id(const std::string& text) {
    std::string id;
    for (char c : trim(text)) {
        if (std::isdigit(static_cast<unsigned char>(c))) id.push_back(c);
        else if (c != '<' && c != '>' && c != '@' && c != '!') return {};
    }
    return id.size() >= 15 && id.size() <= 20 ? id : std::string();
}

std::string format_bytes(uint64_t bytes) {
    const double b = static_cast<double>(bytes);
    if (b >= 1024.0 * 1024 * 1024) return format_double(b / (1024.0 * 1024 * 1024), 1) + " GB";
    if (b >= 1024.0 * 1024) return format_double(b / (1024.0 * 1024), 1) + " MB";
    return format_double(b / 1024.0, 0) + " KB";
}

AdminService::AdminService(Database& db, Repository& repo, MonitorService& monitor, const Config& config, std::string config_path)
    : db_(db), repo_(repo), monitor_(monitor), config_(config), config_path_(std::move(config_path)) {}

void AdminService::set_owners(const std::vector<std::string>& ids) {
    std::lock_guard<std::mutex> lock(owners_mutex_);
    owners_ = ids;
}

std::vector<std::string> AdminService::admins() {
    std::vector<std::string> out;
    {
        std::lock_guard<std::mutex> lock(owners_mutex_);
        out = owners_;
    }
    for (const auto& id : repo_.admins()) {
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    return out;
}

bool AdminService::is_admin(const std::string& user) {
    const auto all = admins();
    return std::find(all.begin(), all.end(), user) != all.end();
}

AdminService::Disk AdminService::disk_space() {
    Disk d;
    if (db_.path() == ":memory:") return d;
    std::error_code ec;
    const auto info = fs::space(utf8_path(db_.path()).parent_path(), ec);
    if (ec) return d;
    d.known = true;
    d.free = info.available;
    d.capacity = info.capacity;
    return d;
}

std::string AdminService::status_text() {
    const Disk disk = disk_space();
    const uint64_t threshold = static_cast<uint64_t>(std::max(0, config_.low_disk_warning_mb)) * 1024 * 1024;
    const CycleStats cycle = monitor_.last_cycle();
    std::vector<std::string> owners;
    {
        std::lock_guard<std::mutex> lock(owners_mutex_);
        owners = owners_;
    }
    std::ostringstream m;
    m << "**Strojanka admin panel** (version " << STROJANKA_VERSION << ")\n"
      << "**Database:** `" << db_.path() << "` (" << format_bytes(db_.file_size()) << ")\n";
    if (disk.known) {
        m << "**Disk:** " << format_bytes(disk.free) << " free of " << format_bytes(disk.capacity)
          << (disk.free < threshold ? "  \xE2\x9A\xA0\xEF\xB8\x8F **LOW**, consider `admin movedb <folder on a bigger drive>`" : "")
          << " (warning below " << config_.low_disk_warning_mb << " MB)\n";
    }
    m << "**Users:** " << repo_.count("users") << " | **watched:** " << repo_.count("watchlist")
      << " | **trades:** " << repo_.count("transactions") << " | **alerts:** " << repo_.count("signals")
      << " | **headlines stored:** " << repo_.count("news_items") << "\n"
      << "**Last price check:** " << ago(cycle.finished_at);
    if (cycle.total) m << ", " << cycle.refreshed << "/" << cycle.total << " stocks refreshed";
    if (cycle.failed_cycles_in_a_row > 0) m << " \xE2\x9A\xA0\xEF\xB8\x8F last error: " << cycle.last_error;
    m << "\n**News weight:** " << format_double(monitor_.news_weight(), 3) << "\n**Admins:** ";
    const auto all = admins();
    if (all.empty()) m << "none (the bot owner becomes admin automatically once connected)";
    for (size_t i = 0; i < all.size(); ++i) {
        const bool owner = std::find(owners.begin(), owners.end(), all[i]) != owners.end();
        m << (i ? ", " : "") << "<@" << all[i] << ">" << (owner ? " (owner)" : "");
    }
    const std::string channel = repo_.meta(kPublicChannelKey);
    m << "\n**Arena channel:** " << (channel.empty() ? "not set (public bots cannot report)" : "<#" + channel + ">");
    const std::string last = last_backup_at();
    const bool overdue = last.empty() || unix_now() - iso_to_unix(last) >= kBackupReminderAfterSeconds;
    m << "\n**Last backup:** " << ago(last) << (overdue ? " \xE2\x9A\xA0\xEF\xB8\x8F" : "");
    if (!last.empty()) m << " (`" << repo_.meta(kLastBackupPathKey) << "`)";
    m << " | **reminders:** "
      << (trim(config_.backup_email).empty() ? "Discord only (add an email with `admin email you@example.com`)" : "Discord and " + config_.backup_email);

    // Markets of the stocks being watched: their prices are only checked while they're open.
    std::vector<const Market*> watched_markets;
    for (const auto& symbol : repo_.watched_symbols()) {
        const Market* market = market_for_symbol(symbol);
        if (market && std::find(watched_markets.begin(), watched_markets.end(), market) == watched_markets.end()) watched_markets.push_back(market);
    }
    if (!watched_markets.empty()) {
        m << "\n**Markets of watched stocks** (" << local_utc_label(unix_now()) << "):";
        for (size_t i = 0; i < watched_markets.size(); ++i) {
            const MarketRow row = market_row(*watched_markets[i], unix_now());
            m << (i ? "; " : " ") << watched_markets[i]->name << " " << (row.open ? "**" + row.status + "**" : row.status);
        }
        m << " (all: `/market`)";
    }
    m << "\n\n`admin add <user ID>` | `admin remove <user ID>` | `admin movedb <folder>` | `admin channel <channel ID>` | "
         "`admin backup` | `admin email <address>`";
    return m.str();
}

CommandReply AdminService::move_database(const std::string& folder_text) {
    const std::string folder = trim(folder_text);
    if (folder.empty()) return {false, "Usage: `admin movedb D:\\StrojankaData`"};
    const fs::path dir = utf8_path(folder);
    if (!dir.is_absolute()) return {false, "Give a full folder path, for example `D:\\StrojankaData`."};
    if (db_.path() == ":memory:") return {false, "This database is in memory and cannot be moved."};

    const fs::path old_path = utf8_path(db_.path());
    const fs::path new_path = dir / old_path.filename();
    std::error_code ec;
    if (fs::equivalent(old_path.parent_path(), dir, ec)) return {false, "The database is already in that folder."};
    fs::create_directories(dir, ec);
    if (ec) return {false, "Cannot create `" + folder + "`: " + ec.message()};
    const uint64_t size = db_.file_size();
    const auto space = fs::space(dir, ec);
    if (!ec && space.available < size + size / 2 + 50ull * 1024 * 1024) {
        return {false, "Not enough room in `" + folder + "`: the database needs about " + format_bytes(size + size / 2) +
                           " and only " + format_bytes(space.available) + " is free."};
    }

    try {
        db_.move_to(path_utf8(new_path));
    } catch (const std::exception& e) {
        return {false, std::string("The move failed and the database stays where it was: ") + e.what()};
    }
    log_info("database moved from " + path_utf8(old_path) + " to " + path_utf8(new_path));

    std::string note;
    std::string error;
    if (auto cfg = load_config(config_path_, &error)) {
        cfg->database_path = path_utf8(new_path);
        if (!save_config(*cfg, config_path_, &error)) note = "\n**Could not update the settings file** (" + error + "). Set `database_path=" + path_utf8(new_path) + "` yourself before the next restart.";
    } else {
        note = "\n**Could not read the settings file** (" + error + "). Set `database_path=" + path_utf8(new_path) + "` yourself before the next restart.";
    }
    bool backups = false;
    for (const auto& e : fs::directory_iterator(old_path.parent_path(), ec)) {
        if (starts_with(path_utf8(e.path().filename()), path_utf8(old_path.filename()) + ".backup-")) backups = true;
    }
    return {true, "Database moved to `" + path_utf8(new_path) + "` (" + format_bytes(size) +
                      "). The copy was checked before the old file was deleted, and the settings now point to the new place." +
                      (backups ? "\nOlder upgrade backups (`*.backup-*`) are still in `" + path_utf8(old_path.parent_path()) +
                                     "`; move or delete them if you need the space."
                               : std::string()) +
                      note};
}

CommandReply AdminService::handle(const std::string& user, const std::vector<std::string>& args) {
    if (!is_admin(user)) return {false, "Only Strojanka admins can use this command."};
    return run(user, args);
}

CommandReply AdminService::handle_console(const std::vector<std::string>& args) { return run("console", args); }

CommandReply AdminService::run(const std::string& user, const std::vector<std::string>& args) {
    const std::string sub = args.empty() ? "status" : to_lower(args[0]);
    auto rest = [&args]() {
        std::string s;
        for (size_t i = 1; i < args.size(); ++i) s += (i > 1 ? " " : "") + args[i];
        return s;
    };
    if (sub == "status" || sub == "panel") return {true, status_text()};
    if (sub == "list") {
        std::string s = "**Admins:**";
        for (const auto& a : admins()) s += " <@" + a + ">";
        return {true, s};
    }
    if (sub == "add" || sub == "remove") {
        const std::string id = parse_user_id(rest());
        if (id.empty()) return {false, "Give a Discord user ID, for example `admin " + sub + " 355014703254339595`. "
                                       "(Developer Mode on, then right-click the person, Copy User ID.)"};
        if (sub == "add") {
            return repo_.add_admin(id, user) ? CommandReply{true, "<@" + id + "> is now an admin and will get warnings."}
                                             : CommandReply{true, "<@" + id + "> was already an admin."};
        }
        {
            std::lock_guard<std::mutex> lock(owners_mutex_);
            if (std::find(owners_.begin(), owners_.end(), id) != owners_.end()) {
                return {false, "<@" + id + "> owns the bot application and is always an admin."};
            }
        }
        return repo_.remove_admin(id) ? CommandReply{true, "<@" + id + "> is no longer an admin."}
                                      : CommandReply{false, "<@" + id + "> was not an admin."};
    }
    if (sub == "movedb") return move_database(rest());
    if (sub == "backup") {
        const BackupResult b = create_backup();
        return b.ok ? CommandReply{true, "Backup saved on the server: `" + b.path + "` (" + format_bytes(b.bytes) + ")."}
                    : CommandReply{false, "The backup failed: " + b.error};
    }
    if (sub == "email") return set_email(rest());
    if (sub == "channel") {
        std::string text = rest();
        text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return c == '<' || c == '>' || c == '#'; }), text.end());
        if (trim(text).empty()) {
            const std::string current = repo_.meta(kPublicChannelKey);
            return {true, current.empty() ? "No public channel is set, so arena bots and politician trades cannot report yet. "
                                            "Set one with `admin channel <channel ID>` (Developer Mode on, right-click the "
                                            "channel, Copy Channel ID)."
                                          : "Public arena bots and politician trades report in <#" + current + ">."};
        }
        const std::string id = parse_user_id(text);  // channel IDs look the same as user IDs
        if (id.empty()) return {false, "Give a channel ID, for example `admin channel 1328815839423758388`."};
        repo_.set_meta(kPublicChannelKey, id);
        return {true, "Public arena bots and politician trades will report in <#" + id + ">. The bot needs permission to "
                      "send messages there."};
    }
    return {false, "Admin commands: `admin`, `admin add <user ID>`, `admin remove <user ID>`, `admin list`, "
                   "`admin movedb <folder>`, `admin channel <channel ID>`, `admin backup`, `admin email <address>|test|off`."};
}

std::string AdminService::backup_folder() const {
    return resolve_config_path(config_path_, trim(config_.backup_folder).empty() ? "backups" : config_.backup_folder);
}

std::string AdminService::last_backup_at() { return repo_.meta(kLastBackupKey); }

BackupResult AdminService::create_backup() {
    BackupResult r;
    if (db_.path() == ":memory:") {
        r.error = "this database is in memory and can't be backed up";
        return r;
    }
    std::string stamp = utc_now_iso();
    for (char& c : stamp) {
        if (c == ':') c = '-';
    }
    fs::path file = utf8_path(backup_folder()) / ("strojanka-" + stamp + kBackupExtension);
    std::error_code exists_ec;
    for (int n = 2; fs::exists(file, exists_ec); ++n) {  // two backups within the same second
        file = utf8_path(backup_folder()) / ("strojanka-" + stamp + "-" + std::to_string(n) + kBackupExtension);
    }
    try {
        db_.backup_to(path_utf8(file));
    } catch (const std::exception& e) {
        r.error = e.what();
        log_warn("backup failed: " + r.error);
        return r;
    }
    std::error_code ec;
    r.ok = true;
    r.path = path_utf8(file);
    r.bytes = fs::file_size(file, ec);
    repo_.set_meta(kLastBackupKey, utc_now_iso());
    repo_.set_meta(kLastBackupPathKey, r.path);
    log_info("backup saved to " + r.path + " (" + format_bytes(r.bytes) + ")");
    return r;
}

CommandReply AdminService::set_email(const std::string& text) {
    const std::string address = trim(text);
    if (address.empty()) {
        return {true, trim(config_.backup_email).empty()
                          ? "No email is set for backup reminders. Set one with `admin email you@example.com`, and put the "
                            "mailbox's password in `smtp_password` in strojanka.conf (Gmail: an app password)."
                          : "Backup reminders also go to **" + config_.backup_email + "**. `admin email test` sends a test message, "
                            "`admin email off` stops them."};
    }
    if (to_lower(address) == "test") {
        std::string error;
        if (!send_email(config_, "Strojanka test message",
                        "This is a test from Strojanka. Backup reminders will arrive like this when the database hasn't been "
                        "backed up for 3 weeks.",
                        &error)) {
            return {false, "The test email could not be sent: " + error};
        }
        return {true, "A test email was sent to **" + config_.backup_email + "**."};
    }
    const bool off = to_lower(address) == "off";
    if (!off && (address.find('@') == std::string::npos || address.find(' ') != std::string::npos)) {
        return {false, "`" + address + "` doesn't look like an email address."};
    }
    std::string error;
    auto cfg = load_config(config_path_, &error);
    if (!cfg) return {false, "Could not read the settings file: " + error};
    cfg->backup_email = off ? "" : address;
    if (!save_config(*cfg, config_path_, &error)) return {false, "Could not save the settings file: " + error};
    config_.backup_email = cfg->backup_email;
    if (off) return {true, "Backup reminders now only go to admins in Discord."};
    std::string note;
    if (cfg->smtp_password.empty()) {
        note = " Put the mailbox's password in `smtp_password` in strojanka.conf (Gmail: create an app password), restart the "
               "bot, then try `admin email test`.";
    }
    if (trim(cfg->smtp_server).empty() && guess_smtp_server(address).empty()) {
        note += " The mail server for this address isn't known: set `smtp_server` (for example `smtps://smtp.example.com:465`).";
    }
    return {true, "Backup reminders will also go to **" + address + "**." + note};
}

void AdminService::remind_backup(Notifier& notifier) {
    const int64_t now = unix_now();
    std::string since = repo_.meta(kBackupSinceKey);
    if (since.empty()) {
        since = utc_now_iso();
        repo_.set_meta(kBackupSinceKey, since);
    }
    const std::string last = last_backup_at();
    const std::string counted_from = last.empty() ? since : last;
    if (now - iso_to_unix(counted_from) < kBackupReminderAfterSeconds) return;
    const std::string reminded = repo_.meta(kBackupRemindedKey);
    const bool first = reminded.empty() || iso_to_unix(reminded) < iso_to_unix(counted_from);
    if (!first && now - iso_to_unix(reminded) < kBackupReminderRepeatSeconds) return;

    const int64_t days = (now - iso_to_unix(counted_from)) / 86400;
    const std::string body = (last.empty() ? "The Strojanka database hasn't been backed up yet (" + std::to_string(days) + " days)."
                                           : "The Strojanka database was last backed up " + std::to_string(days) + " days ago.") +
                             " Make a backup: press Backup in the Strojanka Server window, type `backup` in its console, or send "
                             "`admin backup` to the bot in Discord. Backups are saved in " + backup_folder() +
                             "; copy them to another drive or a cloud folder to be safe.";
    warn_admins(notifier, "\xF0\x9F\x92\xBE **Backup needed.** " + body);
    if (!trim(config_.backup_email).empty()) {
        std::string error;
        if (send_email(config_, "Strojanka: time for a backup", body + "\n\nThis reminder repeats every 2 days until a backup is made.", &error)) {
            log_info("backup reminder emailed to " + config_.backup_email);
        } else {
            log_warn("backup reminder email failed: " + error);
        }
    }
    repo_.set_meta(kBackupRemindedKey, utc_now_iso());
}

void AdminService::warn_admins(Notifier& notifier, const std::string& message) {
    const auto all = admins();
    if (all.empty()) log_warn("no admins to warn: " + message);
    for (const auto& id : all) notifier.send_direct(id, message);
}

void AdminService::check_health(Notifier& notifier) {
    const Disk disk = disk_space();
    const uint64_t threshold = static_cast<uint64_t>(std::max(0, config_.low_disk_warning_mb)) * 1024 * 1024;
    if (disk.known && disk.free < threshold) {
        if (due(repo_.meta(kDiskWarnedKey), kDiskWarningRepeatSeconds)) {
            log_warn("low disk space: " + format_bytes(disk.free) + " free");
            warn_admins(notifier, "\xE2\x9A\xA0\xEF\xB8\x8F **Low disk space.** Only " + format_bytes(disk.free) +
                                      " is free on the drive that holds the Strojanka database (`" + db_.path() + "`, " +
                                      format_bytes(db_.file_size()) + "). If the drive fills up, the bot can no longer save "
                                      "prices, trades or alerts.\nMove the database to a bigger drive without stopping the bot: "
                                      "`admin movedb D:\\StrojankaData`");
            repo_.set_meta(kDiskWarnedKey, utc_now_iso());
        }
    } else if (disk.known && disk.free > threshold + threshold / 5) {
        repo_.set_meta(kDiskWarnedKey, "");
    }

    const CycleStats cycle = monitor_.last_cycle();
    if (cycle.failed_cycles_in_a_row >= 2 && due(repo_.meta(kOutageWarnedKey), kOutageWarningRepeatSeconds)) {
        warn_admins(notifier, "\xE2\x9A\xA0\xEF\xB8\x8F **Price data unavailable.** The last " +
                                  std::to_string(cycle.failed_cycles_in_a_row) +
                                  " price checks could not refresh any stock, so no alerts can be sent. Last error: " +
                                  cycle.last_error + "\nCheck the computer's internet connection.");
        repo_.set_meta(kOutageWarnedKey, utc_now_iso());
    }
    remind_backup(notifier);
}

}  // namespace strojanka
