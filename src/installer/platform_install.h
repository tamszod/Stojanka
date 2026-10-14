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
std::string pick_file(void* owner, const std::string& initial);

// Windows Start menu / desktop shortcuts and the "Apps & features" entry.
void create_shortcuts(const std::string& install_dir, bool start_menu, bool desktop);
void remove_shortcuts();
void register_app(const std::string& install_dir, const std::string& version, unsigned long size_kb);
void unregister_app();

// Deletes `path` a few seconds after this process exits (used when uninstalling the running copy).
void delete_after_exit(const std::string& path);

}  // namespace strojanka::installer::os
