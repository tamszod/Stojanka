#include "installer/installer_core.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "common/payload.h"
#include "common/strings.h"
#include "common/update_backup.h"
#include "common/utf8_path.h"
#include "core/version.h"
#include "installer/platform_install.h"

namespace fs = std::filesystem;

namespace strojanka::installer {

namespace {

void write_launcher(const fs::path& dir) {
    const fs::path p = dir / kLauncher;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
#if defined(_WIN32)
    f << "@echo off\r\n"
      << "cd /d \"%~dp0\"\r\n"
      << "title Strojanka\r\n"
      << "strojanka.exe --config strojanka.conf\r\n"
      << "pause\r\n";
#else
    f << "#!/bin/sh\n"
      << "cd \"$(dirname \"$0\")\"\n"
      << "./strojanka --config strojanka.conf\n";
    f.close();
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec, fs::perm_options::replace, ec);
#endif
}

bool inside(const fs::path& child, const fs::path& parent) {
    const auto c = fs::weakly_canonical(child), p = fs::weakly_canonical(parent);
    auto ci = c.begin();
    for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci) {
        if (ci == c.end() || *ci != *pi) return false;
    }
    return true;
}

// A running copy locks the program files. The window is asked to close (it stops the bot cleanly
// first, which can take up to a minute while the AI model trains); a console copy can't be asked.
std::string stop_running_copy(const fs::path& dir, const ProgressFn& progress) {
    const bool bot_busy = os::file_in_use(path_utf8(dir / kBotExecutable));
    const bool server_busy = os::file_in_use(path_utf8(dir / kServerApp));
    if (!bot_busy && !server_busy) return {};
    if (server_busy) {
        progress(0.03, "Closing the running Strojanka Server (it stops the bot cleanly first)");
        os::close_running_server(path_utf8(dir), 90);
    }
    if (os::file_in_use(path_utf8(dir / kServerApp))) {
        return "Strojanka Server is still running from this folder. Close its window, then try again.";
    }
    if (os::file_in_use(path_utf8(dir / kBotExecutable))) {
        return "Strojanka is running from this folder in a console window (start_strojanka). Close it, then try again.";
    }
    return {};
}

bool same_file(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::exists(a) && fs::exists(b) && fs::equivalent(a, b, ec);
}

unsigned long folder_size_kb(const fs::path& dir) {
    std::error_code ec;
    uintmax_t total = 0;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    }
    return static_cast<unsigned long>(total / 1024);
}

void remove_file(const fs::path& p) {
    std::error_code ec;
    fs::remove(p, ec);
}

// Removes empty folders below and including `dir`, deepest first. Never touches non-empty ones.
void prune_empty(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_directory(ec)) prune_empty(e.path());
    }
    if (fs::is_empty(dir, ec)) fs::remove(dir, ec);
}

Result uninstall(const Request& req, const std::string& container, const ProgressFn& progress) {
    Result r;
    const fs::path dir = fs::absolute(utf8_path(req.dir));
    progress(0.02, "Checking that Strojanka is not running");
    if (const std::string busy = stop_running_copy(dir, progress); !busy.empty()) {
        r.error = busy;
        return r;
    }
    const ExistingInstall existing = inspect_install(req.dir);

    progress(0.2, "Removing program files");
    std::vector<payload::Entry> entries;
    std::string error;
    if (payload::read_index(container, entries, &error)) {
        for (const auto& e : entries) remove_file(dir / utf8_path(e.path));
    }
    remove_file(dir / kLauncher);
    remove_file(dir / "VERSION");
    remove_file(dir / "CHANGELOG.md");

    progress(0.5, "Removing shortcuts");
    os::remove_shortcuts();
    os::set_autostart(path_utf8(dir), false);
    os::unregister_app();

    if (req.remove_data) {
        progress(0.7, "Removing your data");
        remove_file(dir / kConfigName);
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string name = path_utf8(e.path().filename());
            // .log covers strojanka.log; crash diagnostics (see common/crash_handler.*) aren't
            // named with .log alone (the .dmp side file isn't), so match the whole family too.
            if (e.path().extension() == ".log" || starts_with(name, "strojanka-crash")) remove_file(e.path());
        }
        // Only data inside the install folder is deleted; a database kept elsewhere stays.
        if (!existing.database_path.empty() && inside(utf8_path(existing.database_path), dir)) {
            const fs::path db = utf8_path(existing.database_path);
            const fs::path data_dir = db.parent_path();
            for (const auto& e : fs::directory_iterator(data_dir, ec)) {
                const std::string name = path_utf8(e.path().filename());
                if (starts_with(name, path_utf8(db.filename())) || name == "trusted_roots.pem") remove_file(e.path());
            }
        }
    }

    progress(0.9, "Cleaning up");
    if (kSetupCopy[0] != '\0') {
        const fs::path setup = dir / kSetupCopy;
        if (same_file(setup, utf8_path(container))) os::delete_after_exit(path_utf8(setup));
        else remove_file(setup);
    }
    remove_update_backup(path_utf8(dir));
    prune_empty(dir);
    progress(1.0, "Strojanka was removed");
    r.ok = true;
    return r;
}

// Saves the installed version's program files under previous/ before an update overwrites them.
// Which files: the old setup copy's own list when it is there (it is the old installer), else
// the files this update will write. Settings and data stay where they are; they aren't replaced.
bool save_update_backup(const fs::path& dir, const std::string& old_version, const std::vector<payload::Entry>& new_entries,
                        UpdateBackup& b, std::string* error) {
    std::set<std::string> files;
    std::vector<payload::Entry> old_entries;
    if (kSetupCopy[0] != '\0' && payload::read_index(path_utf8(dir / kSetupCopy), old_entries, nullptr)) {
        for (const auto& e : old_entries) files.insert(e.path);
    } else {
        for (const auto& e : new_entries) files.insert(e.path);
    }
    files.insert(kLauncher);
    if (kSetupCopy[0] != '\0') files.insert(kSetupCopy);

    remove_update_backup(path_utf8(dir));
    const fs::path prev = dir / kUpdateBackupDir;
    std::error_code ec;
    b = UpdateBackup{};
    b.version = old_version;
    b.created_at = utc_now_iso();
    b.expires_at = unix_to_iso(unix_now() + static_cast<int64_t>(kRevertDays) * 86400);
    for (const auto& rel : files) {
        const fs::path src = dir / utf8_path(rel);
        if (!fs::is_regular_file(src, ec)) continue;
        const fs::path dst = prev / utf8_path(rel);
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            if (error) *error = "Could not keep a copy of " + rel + " for going back later (" + ec.message() + "). Nothing was changed.";
            remove_update_backup(path_utf8(dir));
            return false;
        }
        b.files.push_back(rel);
    }
    for (const auto& e : new_entries) {
        if (!files.count(e.path) && !fs::exists(dir / utf8_path(e.path), ec)) b.added_files.push_back(e.path);
    }
    return write_update_backup(path_utf8(dir), b, error);
}

// "database backed up to <path>" in the output of `strojanka --migrate` (core/migrations.cpp).
std::string migration_backup_path(const std::string& output) {
    const std::string marker = "database backed up to ";
    const size_t p = output.find(marker);
    if (p == std::string::npos) return {};
    const size_t start = p + marker.size();
    const size_t end = output.find_first_of("\r\n", start);
    return trim(output.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

Result revert(const Request& req, const std::string& container, const ProgressFn& progress) {
    Result r;
    auto fail = [&r](const std::string& message) {
        r.error = message;
        return r;
    };
    const fs::path dir = fs::absolute(utf8_path(req.dir));
    progress(0.02, "Checking that Strojanka is not running");
    if (const std::string busy = stop_running_copy(dir, progress); !busy.empty()) return fail(busy);
    const auto b = read_update_backup(path_utf8(dir));
    if (!b) return fail("There is no earlier version to go back to: no update was made in the last " + std::to_string(kRevertDays) + " days.");
    if (update_backup_expired(*b, unix_now())) {
        remove_update_backup(path_utf8(dir));
        return fail("Going back to version " + b->version + " was possible until " + b->expires_at.substr(0, 10) + ".");
    }
    const fs::path prev = dir / kUpdateBackupDir;
    std::error_code ec;
    for (const auto& rel : b->files) {
        if (!fs::is_regular_file(prev / utf8_path(rel), ec)) {
            return fail("The saved copy of version " + b->version + " is incomplete (" + rel + " is missing), so nothing was changed.");
        }
    }
    if (!b->database_backup.empty() && !fs::is_regular_file(utf8_path(b->database_backup), ec)) {
        return fail("Version " + b->version + " can't open the database the update upgraded, and the copy from before the update (" +
                    b->database_backup + ") is gone, so nothing was changed.");
    }

    progress(0.2, "Restoring version " + b->version);
    for (const auto& rel : b->files) {
        const fs::path dest = dir / utf8_path(rel);
        if (same_file(dest, utf8_path(container))) {
            // The setup running this is the newer one; Windows allows renaming a running program.
            fs::path moved = dest;
            moved += ".newer";
            fs::remove(moved, ec);
            fs::rename(dest, moved, ec);
            if (ec) return fail("Could not replace the running setup (" + ec.message() + ").");
            os::delete_after_exit(path_utf8(moved));
        }
        fs::create_directories(dest.parent_path(), ec);
        fs::copy_file(prev / utf8_path(rel), dest, fs::copy_options::overwrite_existing, ec);
        if (ec) return fail("Could not restore " + rel + " (" + ec.message() + "). Run the setup again to finish.");
    }
    const std::set<std::string> kept(b->files.begin(), b->files.end());
    for (const auto& rel : b->added_files) {
        if (!kept.count(rel)) remove_file(dir / utf8_path(rel));
    }

    progress(0.7, "Restoring settings");
    const fs::path config_path = dir / kConfigName;
    if (auto c = load_config(path_utf8(config_path), nullptr)) {
        c->installed_version = b->version;  // the token, database and other choices stay as they are now
        save_config(*c, path_utf8(config_path), nullptr);
    }
    if (!b->database_backup.empty()) {
        progress(0.8, "Switching back to the database from before the update");
        std::string stamp = utc_now_iso();
        for (char& c : stamp) {
            if (c == ':') c = '-';
        }
        const fs::path db = utf8_path(b->database_path);
        const fs::path moved = utf8_path(b->database_path + ".after-update-" + stamp);
        fs::rename(db, moved, ec);
        for (const char* side : {"-wal", "-shm"}) {
            fs::path from = db, to = moved;
            from += side;
            to += side;
            if (fs::exists(from, ec)) fs::rename(from, to, ec);
        }
        fs::copy_file(utf8_path(b->database_backup), db, fs::copy_options::overwrite_existing, ec);
        if (ec) return fail("Could not put back the database from before the update (" + ec.message() + "). It is still at " + b->database_backup + ".");
        r.note = "Version " + b->version + " can't read the database the update upgraded, so the copy from before the update (" +
                 b->created_at.substr(0, 10) + ") is in use again. Everything recorded since then is kept in " + path_utf8(moved) + ".";
    }

    progress(0.9, "Finishing");
    os::register_app(path_utf8(dir), b->version, folder_size_kb(dir));
    remove_update_backup(path_utf8(dir));
    progress(1.0, "Version " + b->version + " is back");
    r.ok = true;
    r.version = b->version;
    r.launcher = path_utf8(dir / kServerApp);
    return r;
}

}  // namespace

std::string default_install_dir() {
#if defined(_WIN32)
    const std::string base = os::env("LOCALAPPDATA");
    return path_utf8(utf8_path(base.empty() ? os::env("USERPROFILE") : base) / "Programs" / "Strojanka");
#else
    return path_utf8(utf8_path(os::env("HOME")) / "Applications" / "Strojanka");
#endif
}

int compare_versions(const std::string& a, const std::string& b) {
    const auto pa = split(a, '.'), pb = split(b, '.');
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        const int x = i < pa.size() ? std::atoi(pa[i].c_str()) : 0;
        const int y = i < pb.size() ? std::atoi(pb[i].c_str()) : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

const char* action_name(Action a) {
    switch (a) {
        case Action::Install: return "install";
        case Action::Update: return "update";
        case Action::Repair: return "repair";
        case Action::Uninstall: return "uninstall";
        case Action::Revert: return "revert";
    }
    return "install";
}

ExistingInstall inspect_install(const std::string& dir_text) {
    ExistingInstall x;
    x.suggested = "install";
    if (trim(dir_text).empty()) return x;
    const fs::path dir = fs::absolute(utf8_path(dir_text));
    const fs::path config_path = dir / kConfigName;
    std::error_code ec;
    x.found = fs::exists(dir / kBotExecutable, ec) || fs::exists(config_path, ec);
    if (!x.found) return x;
    const os::Integration now = os::current_integration();
    x.start_menu_shortcut = now.start_menu;
    x.desktop_shortcut = now.desktop;
    x.autostart = now.autostart;

    if (auto c = load_config(path_utf8(config_path), nullptr)) {
        x.has_config = true;
        x.config = *c;
        x.version = c->installed_version;
        x.database_path = resolve_config_path(path_utf8(config_path), c->database_path);
        x.database_exists = fs::exists(utf8_path(x.database_path), ec);
    }
    if (x.version.empty()) {
        std::ifstream v(dir / "VERSION");
        std::getline(v, x.version);
        x.version = trim(x.version);
    }
    const int cmp = x.version.empty() ? 1 : compare_versions(STROJANKA_VERSION, x.version);
    x.suggested = cmp > 0 ? "update" : cmp == 0 ? "repair" : "downgrade";
    remove_expired_update_backup(path_utf8(dir), unix_now());
    if (const auto b = read_update_backup(path_utf8(dir)); b && b->version != x.version) {
        x.revert_version = b->version;
        x.revert_until = b->expires_at;
    }
    return x;
}

Result execute(const Request& req, const std::string& container, const ProgressFn& progress) {
    if (req.action == Action::Uninstall) return uninstall(req, container, progress);
    if (req.action == Action::Revert) return revert(req, container, progress);
    Result r;
    auto fail = [&r](const std::string& message) {
        r.error = message;
        return r;
    };
    if (trim(req.dir).empty()) return fail("Choose an install folder.");
    const fs::path dir = fs::absolute(utf8_path(req.dir));
    const fs::path bot = dir / kBotExecutable;
    const fs::path config_path = dir / kConfigName;

    progress(0.02, "Checking the install folder");
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return fail("Cannot create " + path_utf8(dir) + ": " + ec.message());
    if (const std::string busy = stop_running_copy(dir, progress); !busy.empty()) return fail(busy);

    // Settings
    Config config;
    if (req.keep_settings) {
        std::string error;
        auto existing = load_config(path_utf8(config_path), &error);
        if (!existing) return fail("Could not read the existing settings (" + error + "). Choose new settings instead.");
        config = *existing;
    } else {
        if (trim(req.token).empty()) return fail("Enter your Discord bot token.");
        config.discord_token = trim(req.token);
        const std::string db = trim(req.database_path).empty() ? path_utf8(dir / "data" / "strojanka.db") : trim(req.database_path);
        config.database_path = path_utf8(fs::absolute(utf8_path(db)));
        config.poll_interval_minutes = req.poll_minutes > 0 ? req.poll_minutes : 15;
        config.backup_email = trim(req.backup_email);
        config.smtp_password = req.smtp_password;
    }
    if (!trim(req.restore_backup).empty()) {
        // A backup is a plain SQLite database (core/database.cpp backup_to): check it is one before using it.
        std::ifstream f(utf8_path(trim(req.restore_backup)), std::ios::binary);
        char header[16] = {};
        if (!f.read(header, 16) || std::string(header, 15) != "SQLite format 3") {
            return fail("`" + trim(req.restore_backup) + "` is not a Strojanka backup (or it is damaged).");
        }
    }

    // Program files: only write what is missing or different, so Repair shows what it fixed.
    std::vector<payload::Entry> entries;
    std::string error;
    if (!payload::read_index(container, entries, &error)) return fail("This installer is damaged: " + error);

    // Another version is being replaced: keep it for a few days so the update can be reverted.
    const ExistingInstall existing = inspect_install(path_utf8(dir));
    UpdateBackup backup;
    const bool keeping_backup = existing.found && !existing.version.empty() && existing.version != STROJANKA_VERSION &&
                                req.action != Action::Repair;
    if (keeping_backup) {
        progress(0.04, "Keeping a copy of version " + existing.version + " for " + std::to_string(kRevertDays) + " days");
        if (!save_update_backup(dir, existing.version, entries, backup, &error)) return fail(error);
    }
    uint64_t total = 0, done = 0;
    for (const auto& e : entries) total += e.size;
    for (const auto& e : entries) {
        const fs::path dest = dir / utf8_path(e.path);
        uint64_t hash = 0;
        const bool same = fs::exists(dest, ec) && fs::file_size(dest, ec) == e.size && payload::hash_file(path_utf8(dest), hash) &&
                          hash == e.hash;
        if (same) {
            ++r.files_unchanged;
        } else {
            progress(0.05 + 0.7 * (total ? static_cast<double>(done) / total : 1.0), "Copying " + e.path);
            if (!payload::extract(container, e, path_utf8(dest), &error)) return fail(error);
            ++r.files_written;
        }
        done += e.size;
    }
    write_launcher(dir);

    if (!trim(req.restore_backup).empty()) {
        progress(0.76, "Restoring the database from the backup");
        const fs::path db = utf8_path(config.database_path);
        fs::create_directories(db.parent_path(), ec);
        if (fs::exists(db, ec)) {
            // Never overwrite a database: keep it beside the restored one.
            std::string stamp = utc_now_iso();
            for (char& c : stamp) {
                if (c == ':') c = '-';
            }
            fs::path aside = db;
            aside += ".before-restore-" + stamp;
            fs::rename(db, aside, ec);
            for (const char* side : {"-wal", "-shm"}) {
                fs::path from = db, to = aside;
                from += side;
                to += side;
                if (fs::exists(from, ec)) fs::rename(from, to, ec);
            }
            if (ec) return fail("Could not move the existing database aside (" + ec.message() + ").");
            r.note = "The database that was there is kept as " + path_utf8(aside) + ".";
        }
        fs::copy_file(utf8_path(trim(req.restore_backup)), db, fs::copy_options::none, ec);
        if (ec) return fail("Could not restore the backup (" + ec.message() + ").");
    }

    progress(0.78, "Saving settings");
    config.installed_version = STROJANKA_VERSION;
    if (!save_config(config, path_utf8(config_path), &error)) return fail(error);

    progress(0.82, "Preparing the database");
    std::string output;
    const int rc = os::run_capture(path_utf8(bot), {"--migrate", "--config", path_utf8(config_path)}, output);
    if (rc != 0) return fail("Database setup failed (exit code " + std::to_string(rc) + "):\n" + trim(output));
    if (keeping_backup) {
        // The older version can't open an upgraded database; a revert puts this copy back.
        backup.database_path = resolve_config_path(path_utf8(config_path), config.database_path);
        backup.database_backup = migration_backup_path(output);
        write_update_backup(path_utf8(dir), backup, nullptr);
    }

    progress(0.92, "Creating shortcuts");
    if (kSetupCopy[0] != '\0') {
        const fs::path setup = dir / kSetupCopy;
        if (!same_file(setup, utf8_path(container))) fs::copy_file(utf8_path(container), setup, fs::copy_options::overwrite_existing, ec);
    }
    os::remove_shortcuts();
    os::create_shortcuts(path_utf8(dir), req.start_menu_shortcut, req.desktop_shortcut);
    os::set_autostart(path_utf8(dir), req.autostart);
    os::register_app(path_utf8(dir), STROJANKA_VERSION, folder_size_kb(dir));

    progress(1.0, "Finished");
    r.ok = true;
    r.version = STROJANKA_VERSION;
    r.launcher = path_utf8(dir / kServerApp);
    if (keeping_backup) {
        r.note += std::string(r.note.empty() ? "" : " ") + "Version " + existing.version + " is kept until " + backup.expires_at.substr(0, 10) +
                 ": run this setup again (or Settings > Apps > Strojanka > Modify) and choose Revert update to go back.";
    }
    return r;
}

}  // namespace strojanka::installer
