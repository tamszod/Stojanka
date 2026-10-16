// StrojankaServer: runs the bot inside a window with a live dashboard, the log and a command line.
// strojanka.exe remains the windowless version for servers and scripts; both use BotHost.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "bot/bot_host.h"
#include "bot/session_watch.h"
#include "common/crash_handler.h"
#include "common/platform.h"
#include "common/strings.h"
#include "common/utf8_path.h"
#include "core/log.h"
#include "core/markets.h"
#include "core/thread_pool.h"
#include "core/version.h"
#include "server/server_ui.h"  // generated from ui/server.html: kServerHtml

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

#ifdef WIN32_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN  // webview.h defines it itself
#endif
#include "webview.h"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace strojanka;

namespace {

constexpr size_t kRecentLines = 1000;

const char* level_name(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
        default: return "INFO";
    }
}

void open_with_editor(const fs::path& path, bool folder) {
#if defined(_WIN32)
    if (folder) ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + path.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
#else
    std::system((std::string(folder ? "open " : "open -t ") + "'" + path_utf8(path) + "'").c_str());
#endif
}

void show_error(const std::string& message) {
#if defined(_WIN32)
    MessageBoxW(nullptr, utf8_path(message).wstring().c_str(), L"Strojanka Server", MB_ICONERROR | MB_OK);
#else
    std::fprintf(stderr, "%s\n", message.c_str());
#endif
}

// Log lines collected for the window: the most recent ones for a freshly loaded page, and the
// ones not yet delivered.
struct LogFeed {
    std::mutex mutex;
    std::deque<std::pair<std::string, std::string>> recent;
    std::vector<std::pair<std::string, std::string>> pending;
    bool page_ready = false;

    void add(LogLevel level, const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex);
        recent.emplace_back(level_name(level), line);
        if (recent.size() > kRecentLines) recent.pop_front();
        if (page_ready) pending.emplace_back(level_name(level), line);
    }
};

constexpr const char* kWindowTitle = "Strojanka Server";  // the installer finds the window by it

#if defined(_WIN32)
// Opening the shortcut while Strojanka Server already runs (for example minimized since sign-in)
// shows that window instead of starting a second copy, which the instance lock would refuse.
bool activate_running_copy() {
    struct Search {
        std::wstring exe;
        HWND found = nullptr;
    } search;
    wchar_t self[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH * 4);
    search.exe = self;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* s = reinterpret_cast<Search*>(param);
            wchar_t title[64] = {};
            GetWindowTextW(hwnd, title, 64);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId() || utf8_path(kWindowTitle).wstring() != title) return TRUE;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process) return TRUE;
            wchar_t path[MAX_PATH * 4] = {};
            DWORD size = MAX_PATH * 4;
            const bool same = QueryFullProcessImageNameW(process, 0, path, &size) && _wcsicmp(path, s->exe.c_str()) == 0;
            CloseHandle(process);
            if (!same) return TRUE;
            s->found = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (!search.found) return false;
    ShowWindow(search.found, IsIconic(search.found) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(search.found);
    return true;
}
#else
bool activate_running_copy() { return false; }
#endif

}  // namespace

int main(int argc, char** argv) {
    install_crash_handler();
    std::string config_path;
    bool autostart = false;  // started by Windows at sign-in: minimized, and wait for the network
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config_path = argv[++i];
        else if (a == "--autostart") autostart = true;
    }
    if (activate_running_copy()) return 0;
    if (config_path.empty()) {
        fs::path dir = utf8_path(executable_dir());
        // macOS: the program sits in StrojankaServer.app/Contents/MacOS; the settings are next to the .app.
        if (!fs::exists(dir / "strojanka.conf") && dir.filename() == "MacOS") dir = dir.parent_path().parent_path().parent_path();
        config_path = path_utf8(dir / "strojanka.conf");
    }
    const fs::path install_dir = utf8_path(config_path).parent_path();

    LogFeed feed;
    log_set_listener([&feed](LogLevel level, const std::string& line) { feed.add(level, line); });

    BotHost host(config_path);
    if (autostart) host.set_network_wait(std::chrono::minutes(3));
    std::shared_mutex host_mutex;   // unique: start/stop; shared: commands and status
    std::atomic<int> phase{0};      // 0 idle, 1 starting, 2 stopping
    std::mutex error_mutex;
    std::string last_error;
    ThreadPool lifecycle(1);

    auto start_bot = [&] {
        phase = 1;
        std::unique_lock<std::shared_mutex> lock(host_mutex);
        std::string error;
        const bool ok = host.start(&error);
        {
            std::lock_guard<std::mutex> e(error_mutex);
            last_error = ok ? "" : error;
        }
        phase = 0;
    };
    auto stop_bot = [&] {
        phase = 2;
        std::unique_lock<std::shared_mutex> lock(host_mutex);
        host.stop();
        phase = 0;
    };
    // Queues a stop. A start still waiting for the network holds the lock on the same 1-thread
    // pool, so tell it to give up first or the stop would wait minutes behind it.
    auto request_stop = [&] {
        host.abort_network_wait();
        return lifecycle.submit(stop_bot);
    };
    // The window itself has no Ctrl+C; this is for Windows shutting down, restarting or updating.
    SessionWatch watch([&](const std::string& reason, bool forced) {
        if (forced) {
            std::shared_lock<std::shared_mutex> lock(host_mutex, std::try_to_lock);
            if (lock.owns_lock()) host.note_session_end(reason);
        }
        request_stop().wait_for(SessionWatch::kSessionEndGrace);
    });

    try {
        webview::webview w(false, nullptr);
        w.set_title(kWindowTitle);
        w.set_size(1000, 640, WEBVIEW_HINT_MIN);
        w.set_size(1240, 780, WEBVIEW_HINT_NONE);
#if defined(_WIN32)
        // At sign-in: sit in the taskbar instead of popping up over whatever the user opens first.
        if (autostart && w.window().ok()) ShowWindow(static_cast<HWND>(w.window().value()), SW_SHOWMINNOACTIVE);
#endif
        ThreadPool commands(2);  // declared after `w`: finishes (and stops using it) before `w` is destroyed

        w.bind("serverInfo", [&](const std::string&) {
            return json{{"version", STROJANKA_VERSION}, {"config_path", config_path}}.dump();
        });

        w.bind("recentLogs", [&](const std::string&) {
            std::lock_guard<std::mutex> lock(feed.mutex);
            feed.page_ready = true;
            feed.pending.clear();
            json lines = json::array();
            for (const auto& [lvl, line] : feed.recent) lines.push_back({lvl, line});
            return lines.dump();
        });

        w.bind("serverState", [&](const std::string&) {
            json s;
            {
                std::lock_guard<std::mutex> e(error_mutex);
                s["error"] = last_error;
            }
            std::shared_lock<std::shared_mutex> lock(host_mutex, std::try_to_lock);
            if (!lock.owns_lock() || phase != 0) {
                s["state"] = phase == 2 ? "stopping" : "starting";
                return s.dump();
            }
            if (!host.running()) {
                s["state"] = s["error"].get<std::string>().empty() ? "stopped" : "error";
                return s.dump();
            }
            s["state"] = host.online() ? "online" : "starting";
            s["bot_name"] = host.bot_name();
            s["started_at"] = host.started_at();
            auto& repo = host.repository();
            const auto disk = host.admin().disk_space();
            const CycleStats cycle = host.monitor().last_cycle();
            s["stats"] = {
                {"users", repo.count("users")},
                {"watched_symbols", repo.watched_symbols().size()},
                {"watch_entries", repo.count("watchlist")},
                {"alerts", repo.count("signals")},
                {"headlines", repo.count("news_items")},
                {"news_weight", host.monitor().news_weight()},
                {"db_path", host.database().path()},
                {"db_size", host.database().file_size()},
                {"disk_free", disk.known ? disk.free : 0},
                {"disk_capacity", disk.known ? disk.capacity : 0},
                {"disk_threshold", static_cast<uint64_t>(std::max(0, host.config().low_disk_warning_mb)) * 1024 * 1024},
                {"cycle_at", cycle.finished_at},
                {"cycle_refreshed", cycle.refreshed},
                {"cycle_total", cycle.total},
                {"cycle_failures", cycle.failed_cycles_in_a_row},
                {"last_backup_at", host.admin().last_backup_at()},
                {"last_backup_path", repo.meta("last_backup_path")},
            };
            return s.dump();
        });

        w.bind("serverStocks", [&](const std::string&) {
            json rows = json::array();
            std::shared_lock<std::shared_mutex> lock(host_mutex, std::try_to_lock);
            if (!lock.owns_lock() || !host.running()) return rows.dump();
            for (const auto& r : host.console().watched_stocks()) {
                rows.push_back({{"symbol", r.symbol}, {"name", r.name}, {"short_name", short_company_name(r.name)}, {"price", r.price},
                                {"price_text", format_money(r.price, r.currency)}, {"trend", r.trend},
                                {"category", r.category}, {"watchers", r.watchers}, {"last_signal", r.last_signal}});
            }
            return rows.dump();
        });

        w.bind("serverMarkets", [&](const std::string&) {
            const int64_t now = unix_now();
            std::vector<const Market*> watched;
            {
                std::shared_lock<std::shared_mutex> lock(host_mutex, std::try_to_lock);
                if (lock.owns_lock() && host.running()) {
                    for (const auto& symbol : host.repository().watched_symbols()) {
                        if (const Market* m = market_for_symbol(symbol)) watched.push_back(m);
                    }
                }
            }
            json rows = json::array();
            for (const auto& m : markets()) {
                const MarketRow r = market_row(m, now);
                rows.push_back({{"name", m.name}, {"suffix", m.suffix}, {"example", m.example}, {"hours", r.hours}, {"open", r.open},
                                {"status", r.status}, {"watched", std::find(watched.begin(), watched.end(), &m) != watched.end()}});
            }
            return json{{"rows", rows}, {"zone", local_utc_label(now)}}.dump();
        });

        w.bind("arenaBots", [&](const std::string&) {
            json rows = json::array();
            std::shared_lock<std::shared_mutex> lock(host_mutex, std::try_to_lock);
            if (!lock.owns_lock() || !host.running()) return rows.dump();
            for (const auto& v : host.arena().view()) {
                rows.push_back({{"name", v.name}, {"origin", v.origin}, {"idea", v.idea}, {"stage", v.stage},
                                {"stage_label", v.stage_label}, {"value", v.value}, {"live_return", v.live_return},
                                {"vs_atlas", v.vs_atlas}, {"live_trades", v.live_trades}, {"live_drawdown", v.live_drawdown},
                                {"passed", v.backtest_passed}, {"verdict", v.verdict}, {"bt_cagr", v.bt_cagr},
                                {"bt_sharpe", v.bt_sharpe}, {"bt_drawdown", v.bt_drawdown}});
            }
            return rows.dump();
        });

        w.bind("startBot", [&](const std::string&) {
            lifecycle.submit(start_bot);
            return std::string("true");
        });
        w.bind("stopBot", [&](const std::string&) {
            request_stop();
            return std::string("true");
        });

        // Commands can take seconds (check, screen), so they run off the window thread and
        // answer the page's promise when done.
        w.bind(
            "runCommand",
            [&](const std::string& id, const std::string& req, void*) {
                const json args = json::parse(req, nullptr, false);
                const std::string line = args.is_array() && !args.empty() && args[0].is_string() ? args[0].get<std::string>() : "";
                commands.submit([&, id, line] {
                    json reply;
                    try {
                        std::shared_lock<std::shared_mutex> lock(host_mutex);
                        if (!host.running()) {
                            reply = {{"ok", false}, {"text", "The bot is not running. Press **Start** first."}};
                        } else {
                            log_info("console: " + line);
                            const CommandReply r = host.console().run(line);
                            reply = {{"ok", r.ok}, {"text", r.text}};
                        }
                    } catch (const std::exception& e) {
                        reply = {{"ok", false}, {"text", std::string("Error: ") + e.what()}};
                    }
                    w.resolve(id, 0, reply.dump());
                });
            },
            nullptr);

        w.bind("openPath", [&](const std::string& req) {
            const json args = json::parse(req, nullptr, false);
            const std::string what = args.is_array() && !args.empty() && args[0].is_string() ? args[0].get<std::string>() : "";
            if (what == "config") open_with_editor(utf8_path(config_path), false);
            else if (what == "folder") open_with_editor(install_dir, true);
            else if (what == "log") {
                std::string log_file = "strojanka.log";
                if (auto c = load_config(config_path, nullptr); c && !c->log_file.empty()) log_file = c->log_file;
                open_with_editor(utf8_path(resolve_config_path(config_path, log_file)), false);
            }
            return std::string("null");
        });

        std::atomic<bool> running{true};
        std::thread flusher([&] {
            while (running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                std::vector<std::pair<std::string, std::string>> batch;
                {
                    std::lock_guard<std::mutex> lock(feed.mutex);
                    batch.swap(feed.pending);
                }
                if (batch.empty() || !running) continue;
                json lines = json::array();
                for (const auto& [lvl, line] : batch) lines.push_back({lvl, line});
                const std::string js = "window.__logs && window.__logs(" + lines.dump() + ")";
                w.dispatch([&w, js] { w.eval(js); });
            }
        });

        w.set_html(std::string(reinterpret_cast<const char*>(kServerHtml), kServerHtmlSize));
        lifecycle.submit(start_bot);
        w.run();

        running = false;
        flusher.join();
    } catch (const std::exception& e) {
        show_error(std::string("The server window could not be opened (") + e.what() +
                   ").\n\nThe Microsoft Edge WebView2 runtime may be missing. You can still run the bot without a "
                   "window with start_strojanka.bat.");
        log_set_listener({});
        return 1;
    }
    log_set_listener({});
    request_stop().wait();
    return 0;
}
