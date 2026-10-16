#include "installer/console_ui.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "common/strings.h"
#include "common/utf8_path.h"
#include "core/version.h"
#include "installer/installer_core.h"

#if defined(_WIN32)
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace strojanka::installer {

namespace {

std::string ask(const std::string& question, const std::string& fallback) {
    std::cout << question;
    if (!fallback.empty()) std::cout << " [" << fallback << "]";
    std::cout << ": " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return fallback;
    line = trim(line);
    if (line.size() >= 2 && line.front() == '"' && line.back() == '"') line = line.substr(1, line.size() - 2);
    return line.empty() ? fallback : line;
}

bool ask_yes_no(const std::string& question, bool fallback) {
    for (;;) {
        const std::string a = to_lower(ask(question + (fallback ? " (Y/n)" : " (y/N)"), ""));
        if (a.empty()) return fallback;
        if (a == "y" || a == "yes") return true;
        if (a == "n" || a == "no") return false;
    }
}

std::string ask_secret(const std::string& question) {
    std::cout << question << ": " << std::flush;
    std::string value;
#if defined(_WIN32)
    for (;;) {
        const int ch = _getch();
        if (ch == '\r' || ch == '\n' || ch == EOF) break;
        if (ch == 3) std::exit(1);  // Ctrl+C
        if (ch == '\b') {
            if (!value.empty()) value.pop_back();
            continue;
        }
        if (ch == 0 || ch == 0xE0) {
            _getch();  // swallow arrow/function keys
            continue;
        }
        value.push_back(static_cast<char>(ch));
    }
#else
    termios old_tio{};
    const bool tty = tcgetattr(STDIN_FILENO, &old_tio) == 0;
    if (tty) {
        termios tio = old_tio;
        tio.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &tio);
    }
    std::getline(std::cin, value);
    if (tty) tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
#endif
    std::cout << "\n";
    return trim(value);
}

int finish(int code, bool unattended) {
    if (!unattended) ask("\nPress Enter to close", "");
    return code;
}

}  // namespace

int run_console(const std::string& container, const ConsoleOptions& opt) {
    std::cout << "==============================================\n"
              << "  Strojanka " << STROJANKA_VERSION << " setup\n"
              << "  Discord bot for stock PEAK / SUPPORT alerts\n"
              << "==============================================\n\n";

    Request req;
    req.dir = opt.dir;
    if (req.dir.empty()) {
        req.dir = opt.unattended ? default_install_dir() : ask("Install folder", default_install_dir());
    }
    req.dir = path_utf8(fs::absolute(utf8_path(req.dir)));
    const ExistingInstall existing = inspect_install(req.dir);

    if (opt.uninstall) {
        if (!existing.found) {
            std::cerr << "No Strojanka installation in " << req.dir << "\n";
            return finish(1, opt.unattended);
        }
        req.action = Action::Uninstall;
        req.remove_data = opt.remove_data;
        if (!opt.unattended) {
            if (!ask_yes_no("Uninstall Strojanka from " + req.dir + "?", false)) return 1;
            req.remove_data = ask_yes_no("Also delete your database, settings and logs?", false);
        }
    } else if (existing.found && existing.has_config) {
        std::cout << "Found Strojanka " << (existing.version.empty() ? "(unknown version)" : existing.version) << " in this folder.\n";
        bool keep = opt.keep_config || (opt.unattended && opt.token.empty());
        if (!opt.unattended) {
            std::cout << "  1) " << (existing.suggested == "repair" ? "Repair" : "Update") << " and keep the settings\n"
                      << "  2) Reinstall with new settings\n"
                      << "  3) Uninstall\n";
            const std::string choice = ask("Choose 1, 2 or 3", "1");
            if (choice == "3") {
                ConsoleOptions u = opt;
                u.uninstall = true;
                return run_console(container, u);
            }
            keep = choice != "2";
        }
        req.keep_settings = keep;
        req.action = keep ? (existing.suggested == "repair" ? Action::Repair : Action::Update) : Action::Install;
    }

    if (req.action == Action::Install && !req.keep_settings) {
        req.token = opt.token;
        if (req.token.empty() && !opt.unattended) {
            std::cout << "\nDiscord bot token: developer portal -> your app -> Bot -> Reset Token.\n";
            while (req.token.empty()) req.token = ask_secret("Bot token (input is hidden)");
        }
        if (req.token.empty()) {
            std::cerr << "ERROR: a Discord bot token is required (--token)\n";
            return finish(1, opt.unattended);
        }
        const std::string default_db = path_utf8(utf8_path(req.dir) / "data" / "strojanka.db");
        if (!opt.db_existing.empty()) req.database_path = opt.db_existing;
        else if (!opt.db_new.empty()) req.database_path = opt.db_new;
        else if (!opt.unattended) {
            std::cout << "\nDatabase\n  1) Create a new database\n  2) Use an existing database file\n";
            const bool use_existing = ask("Choose 1 or 2", "1") == "2";
            for (;;) {
                req.database_path = ask(use_existing ? "Path of the existing database" : "New database file", use_existing ? "" : default_db);
                if (!use_existing || fs::exists(utf8_path(req.database_path))) break;
                std::cout << "  File not found.\n";
            }
        } else {
            req.database_path = default_db;
        }
        if (!opt.db_existing.empty() && !fs::exists(utf8_path(opt.db_existing))) {
            std::cerr << "ERROR: database not found: " << opt.db_existing << "\n";
            return finish(1, opt.unattended);
        }
        req.poll_minutes = opt.poll > 0 ? opt.poll : 15;
        if (opt.poll <= 0 && !opt.unattended) {
            const int n = std::atoi(ask("Check stocks every how many minutes?", "15").c_str());
            if (n > 0) req.poll_minutes = n;
        }
    }

    // Shortcuts and start-up: keep what is set now (a new install gets the defaults).
    req.start_menu_shortcut = existing.start_menu_shortcut;
    req.desktop_shortcut = existing.desktop_shortcut;
    req.autostart = opt.autostart >= 0 ? opt.autostart == 1 : existing.autostart;
#if defined(_WIN32)
    if (!opt.uninstall && !opt.unattended && opt.autostart < 0) {
        req.autostart = ask_yes_no("Start Strojanka automatically when you sign in to Windows?", req.autostart);
    }
#endif

    if (!opt.unattended && !opt.uninstall && !ask_yes_no(std::string("\nStart: ") + action_name(req.action) + " in " + req.dir + "?", true)) {
        std::cout << "Cancelled.\n";
        return 1;
    }

    std::cout << "\n";
    const Result r = execute(req, container, [](double fraction, const std::string& message) {
        std::cout << "  [" << static_cast<int>(fraction * 100) << "%] " << message << "\n" << std::flush;
    });
    if (!r.ok) {
        std::cerr << "\nERROR: " << r.error << "\n";
        return finish(1, opt.unattended);
    }
    if (req.action == Action::Uninstall) {
        std::cout << "\nStrojanka was removed.\n";
    } else {
        std::cout << "\nStrojanka " << STROJANKA_VERSION << " is ready (" << r.files_written << " file(s) written, "
                  << r.files_unchanged << " already up to date).\n"
                  << "Start it with: " << r.launcher << " (or " << kLauncher << " without a window)\n"
                  << "Then open a direct message with your bot in Discord and type: help\n";
    }
    return finish(0, opt.unattended);
}

}  // namespace strojanka::installer
