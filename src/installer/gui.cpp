#include "installer/gui.h"

#include <atomic>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/version.h"
#include "installer/installer_core.h"
#include "installer/installer_ui.h"  // generated from ui/installer.html: kInstallerHtml
#include "installer/platform_install.h"

#ifdef WIN32_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN  // webview.h defines it itself
#endif
#include "webview.h"

namespace strojanka::installer {

namespace {

using nlohmann::json;

json arg(const std::string& req, size_t index) {
    const json args = json::parse(req, nullptr, false);
    return args.is_array() && index < args.size() ? args[index] : json();
}

std::string str_arg(const std::string& req, size_t index) {
    const json a = arg(req, index);
    return a.is_string() ? a.get<std::string>() : std::string();
}

Action parse_action(const std::string& s) {
    if (s == "update") return Action::Update;
    if (s == "repair") return Action::Repair;
    if (s == "uninstall") return Action::Uninstall;
    return Action::Install;
}

}  // namespace

int run_gui(const std::string& container, const std::string& dir, const std::string& start) {
    try {
        webview::webview w(false, nullptr);
        w.set_title("Strojanka Setup");
        w.set_size(880, 600, WEBVIEW_HINT_FIXED);

        std::thread worker;
        std::atomic<bool> busy{false};
        int exit_code = 1;
        void* window = w.window().ok() ? w.window().value() : nullptr;

        w.bind("appInfo", [&](const std::string&) {
            return json{{"version", STROJANKA_VERSION},
#if defined(_WIN32)
                        {"platform", "windows"},
#else
                        {"platform", "mac"},
#endif
                        {"dir", dir.empty() ? default_install_dir() : dir},
                        {"start", start}}
                .dump();
        });

        w.bind("inspectDir", [&](const std::string& req) {
            const ExistingInstall x = inspect_install(str_arg(req, 0));
            return json{{"found", x.found},
                        {"has_config", x.has_config},
                        {"version", x.version},
                        {"database_path", x.database_path},
                        {"database_exists", x.database_exists},
                        {"poll", x.config.poll_interval_minutes},
                        {"suggested", x.suggested},
                        {"start_menu", x.start_menu_shortcut},
                        {"desktop", x.desktop_shortcut},
                        {"autostart", x.autostart}}
                .dump();
        });

        w.bind("pickFolder", [&](const std::string& req) { return json(os::pick_folder(window, str_arg(req, 0))).dump(); });
        w.bind("pickFile", [&](const std::string& req) { return json(os::pick_file(window, str_arg(req, 0))).dump(); });
        w.bind("openPath", [&](const std::string& req) { return json(os::open_path(str_arg(req, 0))).dump(); });

        w.bind("closeWindow", [&](const std::string&) {
            if (!busy) w.terminate();
            return std::string("null");
        });

        w.bind("startTask", [&](const std::string& req) {
            if (busy.exchange(true)) return std::string("false");
            const json r = arg(req, 0);
            Request request;
            request.action = parse_action(r.value("action", std::string("install")));
            request.dir = r.value("dir", std::string());
            request.keep_settings = r.value("keep_settings", false);
            request.token = r.value("token", std::string());
            request.database_path = r.value("database_path", std::string());
            request.poll_minutes = r.value("poll_minutes", 15);
            request.start_menu_shortcut = r.value("start_menu_shortcut", true);
            request.desktop_shortcut = r.value("desktop_shortcut", false);
            request.autostart = r.value("autostart", false);
            request.remove_data = r.value("remove_data", false);
            if (worker.joinable()) worker.join();
            worker = std::thread([&w, &busy, &exit_code, request, container] {
                auto progress = [&w](double fraction, const std::string& message) {
                    const std::string js = "window.__progress(" + json(fraction).dump() + "," + json(message).dump() + ")";
                    w.dispatch([&w, js] { w.eval(js); });
                };
                Result result;
                try {
                    result = execute(request, container, progress);
                } catch (const std::exception& e) {
                    result.error = e.what();
                }
                exit_code = result.ok ? 0 : 1;
                const json done{{"ok", result.ok},
                                {"error", result.error},
                                {"files_written", result.files_written},
                                {"files_unchanged", result.files_unchanged},
                                {"launcher", result.launcher}};
                const std::string js = "window.__finished(" + done.dump() + ")";
                w.dispatch([&w, &busy, js] {
                    busy = false;
                    w.eval(js);
                });
            });
            return std::string("true");
        });

        w.set_html(std::string(reinterpret_cast<const char*>(kInstallerHtml), kInstallerHtmlSize));
        w.run();
        if (worker.joinable()) worker.join();
        return exit_code;
    } catch (const std::exception&) {
        return -1;
    }
}

}  // namespace strojanka::installer
