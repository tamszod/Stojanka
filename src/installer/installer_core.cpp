#include "installer/installer_core.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "common/payload.h"
#include "common/strings.h"
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
    progress(0.05, "Checking that Strojanka is not running");
    if (os::file_in_use(path_utf8(dir / kBotExecutable)) || os::file_in_use(path_utf8(dir / kServerApp))) {
        r.error = "Strojanka is running. Close the Strojanka Server window (or the start_strojanka console) and try again.";
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
    os::unregister_app();

    if (req.remove_data) {
        progress(0.7, "Removing your data");
        remove_file(dir / kConfigName);
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() == ".log") remove_file(e.path());
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
    prune_empty(dir);
    progress(1.0, "Strojanka was removed");
    r.ok = true;
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
    return x;
}

Result execute(const Request& req, const std::string& container, const ProgressFn& progress) {
    if (req.action == Action::Uninstall) return uninstall(req, container, progress);
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
    if (os::file_in_use(path_utf8(bot)) || os::file_in_use(path_utf8(dir / kServerApp))) {
        return fail("Strojanka is running from this folder. Close the Strojanka Server window (or the start_strojanka "
                    "console), then try again.");
    }

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
    }

    // Program files: only write what is missing or different, so Repair shows what it fixed.
    std::vector<payload::Entry> entries;
    std::string error;
    if (!payload::read_index(container, entries, &error)) return fail("This installer is damaged: " + error);
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

    progress(0.78, "Saving settings");
    config.installed_version = STROJANKA_VERSION;
    if (!save_config(config, path_utf8(config_path), &error)) return fail(error);

    progress(0.82, "Preparing the database");
    std::string output;
    const int rc = os::run_capture(path_utf8(bot), {"--migrate", "--config", path_utf8(config_path)}, output);
    if (rc != 0) return fail("Database setup failed (exit code " + std::to_string(rc) + "):\n" + trim(output));

    progress(0.92, "Creating shortcuts");
    if (kSetupCopy[0] != '\0') {
        const fs::path setup = dir / kSetupCopy;
        if (!same_file(setup, utf8_path(container))) fs::copy_file(utf8_path(container), setup, fs::copy_options::overwrite_existing, ec);
    }
    os::remove_shortcuts();
    os::create_shortcuts(path_utf8(dir), req.start_menu_shortcut, req.desktop_shortcut);
    os::register_app(path_utf8(dir), STROJANKA_VERSION, folder_size_kb(dir));

    progress(1.0, "Finished");
    r.ok = true;
    r.launcher = path_utf8(dir / kServerApp);
    return r;
}

}  // namespace strojanka::installer
