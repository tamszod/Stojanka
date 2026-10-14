#include "core/log.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>

#include "common/strings.h"
#include "common/utf8_path.h"

namespace strojanka {

namespace {
std::mutex g_mutex;
LogLevel g_level = LogLevel::Info;
std::ofstream g_file;
LogListener g_listener;

const char* level_name(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO ";
        case LogLevel::Warn: return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}
}  // namespace

LogLevel parse_log_level(const std::string& name) {
    const auto n = to_lower(trim(name));
    if (n == "debug") return LogLevel::Debug;
    if (n == "warn" || n == "warning") return LogLevel::Warn;
    if (n == "error") return LogLevel::Error;
    return LogLevel::Info;
}

void log_set_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = level;
}

void log_set_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open()) g_file.close();
    if (path.empty()) return;
    std::error_code ec;
    const auto p = utf8_path(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    g_file.open(p, std::ios::app);
}

void log_write(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (level < g_level) return;
    const std::string line = utc_now_iso() + " [" + level_name(level) + "] " + message;
    (level >= LogLevel::Warn ? std::cerr : std::cout) << line << std::endl;
    if (g_file.is_open()) g_file << line << std::endl;
    if (g_listener) g_listener(level, line);
}

LogLevel log_level() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_level;
}

void log_set_listener(LogListener listener) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_listener = std::move(listener);
}

}  // namespace strojanka
