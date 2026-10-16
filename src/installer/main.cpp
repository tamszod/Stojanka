// StrojankaInstaller: a self-extracting installer. The program files are appended to this
// executable by strojanka_pack (see common/payload.h). It shows a windowed installer
// (installer/gui.cpp) and falls back to text mode (installer/console_ui.cpp). It needs no
// DLLs and links the C++ runtime statically, so it runs on a clean machine.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "common/crash_handler.h"
#include "common/platform.h"
#include "core/version.h"
#include "installer/console_ui.h"
#include "installer/gui.h"

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace strojanka;
using namespace strojanka::installer;

namespace {

void print_usage() {
    std::cout << "StrojankaInstaller " << STROJANKA_VERSION << "\n\n"
              << "Without options the windowed installer opens. Other options:\n"
              << "  --console               use the text installer instead of the window\n"
              << "  --yes                   do not ask questions; use the options below and defaults\n"
              << "  --dir <folder>          install folder\n"
              << "  --token <token>         Discord bot token\n"
              << "  --new-db <file>         create a new database at this path\n"
              << "  --existing-db <file>    use (and upgrade) an existing database\n"
              << "  --poll <minutes>        how often stocks are checked (default 15)\n"
              << "  --keep-config           on update, keep the existing strojanka.conf\n"
              << "  --autostart             start Strojanka Server (minimized) when you sign in to Windows\n"
              << "  --no-autostart          don't; without either option the current choice is kept (new installs: on)\n"
              << "  --uninstall             remove Strojanka (add --remove-data to delete the database too)\n"
              << "  --revert                go back to the version the last update replaced (kept for 3 days)\n";
}

#if defined(_WIN32)
// The installer is a GUI program, so it has no console of its own. Text mode borrows the
// console of the shell it was started from, or opens a new one.
void attach_console() {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool redirected = out != nullptr && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN;
    if (redirected) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
    std::ios::sync_with_stdio(true);
    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();
}
#else
void attach_console() {}
#endif

}  // namespace

int main(int argc, char** argv) {
    install_crash_handler();
    enable_utf8_console();
    ConsoleOptions opt;
    bool console = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& target) {
            if (i + 1 >= argc) return false;
            target = argv[++i];
            return true;
        };
        bool ok = true;
        if (a == "--dir") ok = next(opt.dir);
        else if (a == "--token") ok = next(opt.token);
        else if (a == "--new-db") ok = next(opt.db_new);
        else if (a == "--existing-db") ok = next(opt.db_existing);
        else if (a == "--poll") {
            std::string v;
            ok = next(v);
            opt.poll = std::atoi(v.c_str());
        } else if (a == "--keep-config") opt.keep_config = true;
        else if (a == "--yes" || a == "-y") opt.unattended = true;
        else if (a == "--console") console = true;
        else if (a == "--uninstall") opt.uninstall = true;
        else if (a == "--remove-data") opt.remove_data = true;
        else if (a == "--autostart") opt.autostart = 1;
        else if (a == "--no-autostart") opt.autostart = 0;
        else if (a == "--revert") opt.revert = true;
        else ok = false;
        if (!ok) {
            attach_console();
            print_usage();
            return 2;
        }
    }

    const std::string self = executable_path();
    if (!console && !opt.unattended) {
        const int rc = run_gui(self, opt.dir, opt.uninstall ? "uninstall" : opt.revert ? "revert" : "");
        if (rc >= 0) return rc;
    }
    attach_console();
    return run_console(self, opt);
}
