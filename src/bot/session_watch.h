#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace strojanka {

// Notices the ways the process gets ended from outside without crashing: Windows shutting down,
// restarting or installing an update, the user signing out, the console window being closed, or
// Ctrl+C. Without this, all of them end the process with no line in the log, which looks exactly
// like a crash. Also logs sleep and wake-up, which explain gaps in the log.
//
// on_end(reason, forced) runs once, on a watcher thread, after the reason has been logged. forced
// = Windows ends the process soon after on_end returns (it waits only a few seconds, so stop
// within kSessionEndGrace); not forced = an ordinary stop request (Ctrl+C).
// Windows only; elsewhere this does nothing.
class SessionWatch {
public:
    using OnEnd = std::function<void(const std::string& reason, bool forced)>;
    static constexpr std::chrono::seconds kSessionEndGrace{4};

    explicit SessionWatch(OnEnd on_end);
    ~SessionWatch();
    SessionWatch(const SessionWatch&) = delete;
    SessionWatch& operator=(const SessionWatch&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace strojanka
