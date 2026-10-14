#pragma once

#include <string>

namespace strojanka::installer {

struct ConsoleOptions {
    std::string dir;
    std::string token;
    std::string db_new;
    std::string db_existing;
    int poll = 0;
    bool unattended = false;   // --yes: no questions, use options and defaults
    bool keep_config = false;  // --keep-config
    bool uninstall = false;    // --uninstall
    bool remove_data = false;  // --remove-data
};

// Text-mode installer: used with --console / --yes, and when no window can be shown.
int run_console(const std::string& container, const ConsoleOptions& options);

}  // namespace strojanka::installer
