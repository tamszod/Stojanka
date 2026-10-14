#pragma once

namespace strojanka {

// Installs process-wide handlers so a crash (access violation, unhandled C++ exception reaching
// the top of the program, or abort()) is recorded before the process ends, instead of
// disappearing with no trace. Call once, as early as possible in main().
//
// Writes a line to "strojanka-crash.log" next to the executable, and on Windows also a small
// .dmp file next to it that can be opened in Visual Studio or WinDbg for a stack trace. This
// reliably catches access violations and similar hard crashes (the most common real-world
// cause) on any thread. It also catches std::terminate() calls and exceptions that escape
// main() on the main thread, but on Windows an exception that escapes a background thread's
// entry function bypasses every handler (a deliberate MSVC runtime hardening measure) — which
// is why every background job in this codebase already wraps its own body in try/catch.
// Deliberately independent of the application's own logger (core/log.h): that logger takes a
// mutex, and calling it again from a crash handler on the same thread could deadlock if the
// crash happened while the thread already held that lock.
void install_crash_handler();

}  // namespace strojanka
