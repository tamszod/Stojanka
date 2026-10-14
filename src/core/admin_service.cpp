#include "core/admin_service.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

#include "common/strings.h"
#include "core/arena/arena_service.h"
#include "common/utf8_path.h"
#include "core/log.h"
#include "core/version.h"

namespace fs = std::filesystem;

namespace strojanka {

namespace {

constexpr int64_t kDiskWarningRepeatSeconds = 24 * 3600;
constexpr int64_t kOutageWarningRepeatSeconds = 6 * 3600;
constexpr const char* kDiskWarnedKey = "disk_warned_at";
constexpr const char* kOutageWarnedKey = "outage_warned_at";

bool due(const std::string& last_iso, int64_t every) { return last_iso.empty() || unix_now() - iso_to_unix(last_iso) >= every; }

std::string ago(const std::string& iso) {
    if (iso.empty()) return "never";
    const int64_t s = unix_now() - iso_to_unix(iso);
    if (s < 120) return "just now";
    if (s < 7200) return std::to_string(s / 60) + " min ago";
    return std::to_string(s / 3600) + " h ago";
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
    m << "\n\n`admin add <user ID>` | `admin remove <user ID>` | `admin movedb <folder>` | `admin channel <channel ID>`";
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
                   "`admin movedb <folder>`, `admin channel <channel ID>`."};
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
}

}  // namespace strojanka
