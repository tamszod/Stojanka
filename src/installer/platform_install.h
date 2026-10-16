#pragma once

#include <string>
#include <vector>

// Operating-system specific pieces of the installer. Functions that make no sense on a
// platform are harmless no-ops there.
namespace strojanka::installer::os {

std::string env(const char* name);

// True if another process has the file open for execution/writing (Windows: the bot is running).
bool file_in_use(const std::string& path);

// Runs a program without showing a console window and captures its combined output.
int run_capture(const std::string& exe, const std::vector<std::string>& args, std::string& output);

// Opens a file or folder with the default handler (launcher script, Explorer/Finder).
bool open_path(const std::string& path);

// Native pickers. Return "" when cancelled. `owner` is the native window handle or nullptr.
std::string pick_folder(void* owner, const std::string& initial);
std::string pick_file(void* owner, const std::string& initial, bool backup = false);  // backup: .strojanka_backup files

// Windows Start menu / desktop shortcuts and the "Apps & features" entry.
void create_shortcuts(const std::string& install_dir, bool start_menu, bool desktop);
void remove_shortcuts();
void register_app(const std::string& install_dir, const std::string& version, unsigned long size_kb);
void unregister_app();

// What is set up right now, so an update keeps the user's choices.
struct Integration {
    bool start_menu = false;
    bool desktop = false;
    bool autostart = false;
};
Integration current_integration();

// Starts Strojanka Server (minimized) when the user signs in to Windows: a per-user "Run" entry,
// listed (and switchable) under Task Manager > Startup apps. enabled=false removes it.
void set_autostart(const std::string& install_dir, bool enabled);

// Asks a running Strojanka Server window from this folder to close (it stops the bot cleanly) and
// waits until the program file is free. True when nothing from the folder is running anymore.
bool close_running_server(const std::string& install_dir, int timeout_seconds);

// Deletes `path` a few seconds after this process exits (used when uninstalling the running copy).
void delete_after_exit(const std::string& path);

}  // namespace strojanka::installer::os
