#pragma once

#include <string>

namespace strojanka::installer {

// Shows the windowed installer. `start` is "" or "uninstall" (opened from Windows settings).
// Returns the process exit code, or -1 if no window could be created (for example when the
// WebView2 runtime is missing); the caller then falls back to the console installer.
int run_gui(const std::string& container, const std::string& dir, const std::string& start);

}  // namespace strojanka::installer
