#pragma once

#include <functional>
#include <string>
#include <vector>

#include "common/config.h"

// The installer's actual work, independent of how it is presented (window or console).
namespace strojanka::installer {

#if defined(_WIN32)
inline constexpr const char* kBotExecutable = "strojanka.exe";
inline constexpr const char* kServerApp = "StrojankaServer.exe";  // the window most people start
inline constexpr const char* kLauncher = "start_strojanka.bat";   // headless, for servers
inline constexpr const char* kSetupCopy = "StrojankaSetup.exe";  // kept for Modify/Uninstall in Windows settings
#else
inline constexpr const char* kBotExecutable = "strojanka";
inline constexpr const char* kServerApp = "StrojankaServer.app";
inline constexpr const char* kLauncher = "start_strojanka.command";
inline constexpr const char* kSetupCopy = "";
#endif
inline constexpr const char* kConfigName = "strojanka.conf";

std::string default_install_dir();

// -1 if a < b, 0 if equal, 1 if a > b. Compares dot-separated numbers ("0.1.10" > "0.1.9").
int compare_versions(const std::string& a, const std::string& b);

struct ExistingInstall {
    bool found = false;           // a Strojanka program or settings file is in the folder
    bool has_config = false;
    std::string version;          // installed version, empty if unknown
    Config config;
    std::string database_path;    // absolute
    bool database_exists = false;
    std::string suggested;        // "install", "update", "repair" or "downgrade"
    // Windows integration as it is now (for a new install: the defaults offered).
    bool start_menu_shortcut = true;
    bool desktop_shortcut = false;
    bool autostart = true;
    // The version the last update replaced, while it can still be restored ("" = nothing to revert).
    std::string revert_version;
    std::string revert_until;  // ISO UTC
};
// Also deletes an update backup whose days are over.
ExistingInstall inspect_install(const std::string& dir);

enum class Action { Install, Update, Repair, Uninstall, Revert };
const char* action_name(Action a);

struct Request {
    Action action = Action::Install;
    std::string dir;
    bool keep_settings = false;         // Update/Repair: reuse strojanka.conf (token, database, interval)
    std::string token;                  // used when !keep_settings
    std::string database_path;          // new or existing database file (when !keep_settings)
    std::string restore_backup;         // a .strojanka_backup to put at database_path first ("" = none)
    std::string backup_email;           // when !keep_settings: where backup reminders go ("" = Discord only)
    std::string smtp_password;          // that mailbox's (app) password
    int poll_minutes = 15;
    bool start_menu_shortcut = true;    // Windows
    bool desktop_shortcut = false;      // Windows
    bool autostart = false;             // Windows: start Strojanka Server when the user signs in
    bool remove_data = false;           // Uninstall: also delete database, settings and logs
};

struct Result {
    bool ok = false;
    std::string error;
    int files_written = 0;
    int files_unchanged = 0;
    std::string launcher;               // what "Start Strojanka now" opens (the server window)
    std::string version;                // the version now installed
    std::string note;                   // something the user should know, e.g. which database is in use after a revert
};

// fraction is 0..1; message is a short human-readable step description.
using ProgressFn = std::function<void(double fraction, const std::string& message)>;

// `container` is the installer executable holding the payload.
Result execute(const Request& request, const std::string& container, const ProgressFn& progress);

}  // namespace strojanka::installer
