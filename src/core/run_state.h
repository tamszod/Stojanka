#pragma once

#include <cstdint>
#include <string>

namespace strojanka {

// app_meta keys BotHost keeps so a start can say how the previous run ended.
inline constexpr const char* kRunStateKey = "run_state";  // running | stopped | "ended: <reason>"
inline constexpr const char* kRunStartedKey = "run_started_at";

struct PreviousRun {
    bool warn = false;  // ended uncleanly for an unknown reason
    std::string text;   // empty = nothing worth saying
};

// state/started_at as stored under the keys above; boot_unix = when the computer last started (0 = unknown).
PreviousRun describe_previous_run(const std::string& state, const std::string& started_at, int64_t boot_unix);

}  // namespace strojanka
