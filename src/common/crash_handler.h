#pragma once

#include <string>

namespace strojanka {

// Installs process-wide handlers so a crash (access violation, an unhandled C++ exception on any
// thread, std::terminate or abort()) is recorded before the process ends, instead of
// disappearing with no trace. Call once, as early as possible in main().
//
// Writes a line to "strojanka-crash.log" next to the executable, and on Windows also a small
// .dmp file next to it that can be opened in Visual Studio or WinDbg for a stack trace.
// Not catchable in-process: heap corruption and /GS failures (Windows ends those with
// __fastfail), stack overflows, and the process being killed from outside. Windows records
// those in Event Viewer (Application log, "Application Error").
// Deliberately independent of the application's own logger (core/log.h): that logger takes a
// mutex, and calling it again from a crash handler on the same thread could deadlock if the
// crash happened while the thread already held that lock.
void install_crash_handler();

// Also copies crash lines into the main log file (empty = stop), since that's the file people
// look at and attach first.
void set_crash_log_mirror(const std::string& log_path);

}  // namespace strojanka
