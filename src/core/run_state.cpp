#include "core/run_state.h"

#include "common/strings.h"

namespace strojanka {

PreviousRun describe_previous_run(const std::string& state, const std::string& started_at, int64_t boot_unix) {
    PreviousRun r;
    if (state.rfind("ended: ", 0) == 0) {
        r.text = "the previous run was ended from outside before it finished stopping: " + state.substr(7);
        return r;
    }
    if (state != "running") return r;
    r.warn = true;
    r.text = "the previous run";
    if (!started_at.empty()) r.text += " (started " + started_at + ")";
    r.text += " did not shut down cleanly: it crashed, was ended from outside (for example by Task Manager) or "
              "the computer lost power";
    if (boot_unix > 0 && !started_at.empty() && boot_unix > iso_to_unix(started_at)) {
        r.text += ". The computer has restarted since then (at " + unix_to_iso(boot_unix) + ")";
    }
    r.text += ". If strojanka-crash.log is next to the program, it has the details";
    return r;
}

}  // namespace strojanka
