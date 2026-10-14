#include "common/crash_handler.h"

#include <cstdio>
#include <ctime>
#include <exception>
#include <fstream>
#include <string>

#include "common/platform.h"
#include "common/utf8_path.h"

#if defined(_WIN32)
#include <windows.h>

#include <dbghelp.h>
#else
#include <csignal>
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

// Appends one line. Independent of core/log.h's own mutex-protected logger: re-entering that
// logger from a crash handler on the same thread could deadlock if the crash happened inside it.
void append_line(const std::string& message) {
    const std::string line = timestamp() + " " + message;
    std::ofstream f(crash_log_path(), std::ios::app);
    if (f) f << line << "\n";
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
                      static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo), &mei, nullptr, nullptr);
    CloseHandle(file);
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
#else
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) std::signal(sig, signal_handler);
#endif
}

}  // namespace strojanka
