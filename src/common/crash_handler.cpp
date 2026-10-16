#include "common/crash_handler.h"

#include <csignal>
#include <cstdio>
#include <ctime>
#include <exception>
#include <fstream>
#include <mutex>
#include <string>

#include "common/platform.h"
#include "common/utf8_path.h"

#if defined(_WIN32)
#include <windows.h>

#include <dbghelp.h>
#endif

namespace strojanka {

namespace {

std::filesystem::path crash_log_path() { return utf8_path(executable_dir()) / "strojanka-crash.log"; }

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::mutex g_mirror_mutex;
std::filesystem::path g_mirror_path;

// Appends one line. Independent of core/log.h's own mutex-protected logger: re-entering that
// logger from a crash handler on the same thread could deadlock if the crash happened inside it.
void append_line(const std::string& message) {
    const std::string line = timestamp() + " " + message;
    {
        std::ofstream f(crash_log_path(), std::ios::app);
        if (f) f << line << "\n";
    }
    std::filesystem::path mirror;
    if (g_mirror_mutex.try_lock()) {
        mirror = g_mirror_path;
        g_mirror_mutex.unlock();
    }
    if (!mirror.empty()) {
        std::ofstream f(mirror, std::ios::app);
        if (f) f << line.substr(0, 21) << "[ERROR] " << line.substr(21) << "\n";
    }
    std::fprintf(stderr, "%s\n", line.c_str());
}

#if defined(_WIN32)

void write_minidump(EXCEPTION_POINTERS* info) {
    const auto dump_path = utf8_path(executable_dir()) / "strojanka-crash.dmp";
    HANDLE file = CreateFileW(dump_path.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = info;
    mei.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo), info ? &mei : nullptr,
                      nullptr, nullptr);
    CloseHandle(file);
}

// MSVC keeps the std::set_terminate handler per thread, so on any thread but the one that
// installed it an escaping exception runs the default terminate -> abort(). SIGABRT handlers
// are process-wide in the UCRT, so this is the one place every such death passes through.
void abort_handler(int) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "CRASH: the program aborted on thread %lu", GetCurrentThreadId());
    append_line(std::string(buf) +
                " (an unhandled C++ exception on a background thread, std::terminate or abort())");
    write_minidump(nullptr);
    append_line("CRASH: wrote strojanka-crash.dmp next to the program for diagnosis");
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* info) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "CRASH: unhandled exception 0x%08lX at address %p",
                 info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    append_line(buf);
    write_minidump(info);
    append_line("CRASH: wrote strojanka-crash.dmp next to the program for diagnosis");
    return EXCEPTION_CONTINUE_SEARCH;  // Windows still shows/terminates normally afterward
}

#else

void signal_handler(int sig) {
    append_line("CRASH: terminated by signal " + std::to_string(sig));
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

#endif

void terminate_handler() {
    std::string what = "unknown reason";
    if (auto ex = std::current_exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
            what = "a non-standard C++ exception";
        }
    }
    append_line("CRASH: an unhandled exception reached the top of the program: " + what);
    std::abort();
}

}  // namespace

void install_crash_handler() {
    std::set_terminate(terminate_handler);
#if defined(_WIN32)
    SetUnhandledExceptionFilter(unhandled_filter);
    std::signal(SIGABRT, abort_handler);
#else
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) std::signal(sig, signal_handler);
#endif
}

void set_crash_log_mirror(const std::string& log_path) {
    std::lock_guard<std::mutex> lock(g_mirror_mutex);
    g_mirror_path = log_path.empty() ? std::filesystem::path() : utf8_path(log_path);
}

}  // namespace strojanka
