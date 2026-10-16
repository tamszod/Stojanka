#include "common/platform.h"
#include "common/utf8_path.h"

#include <ctime>
#include <filesystem>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <cstdint>
#else
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace strojanka {

std::string executable_path() {
#if defined(_WIN32)
    std::vector<wchar_t> buf(32768);
    const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    return path_utf8(fs::path(std::wstring(buf.data(), len)));
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    const auto p = fs::canonical(fs::path(buf.data()), ec);
    return ec ? std::string(buf.data()) : path_utf8(p);
#else
    std::error_code ec;
    return path_utf8(fs::read_symlink("/proc/self/exe", ec));
#endif
}

std::string executable_dir() { return path_utf8(utf8_path(executable_path()).parent_path()); }

void enable_utf8_console() {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

int64_t system_boot_unix() {
    const auto now = static_cast<int64_t>(std::time(nullptr));
#if defined(_WIN32)
    // Counts time asleep too, like the boot time Windows itself reports.
    return now - static_cast<int64_t>(GetTickCount64() / 1000);
#elif defined(__APPLE__)
    timeval boot{};
    size_t size = sizeof boot;
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boot, &size, nullptr, 0) != 0) return 0;
    return static_cast<int64_t>(boot.tv_sec);
#else
    struct sysinfo info {};
    if (sysinfo(&info) != 0) return 0;
    return now - static_cast<int64_t>(info.uptime);
#endif
}

}  // namespace strojanka
