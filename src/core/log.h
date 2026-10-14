#pragma once

#include <functional>
#include <string>

namespace strojanka {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

LogLevel parse_log_level(const std::string& name);
void log_set_level(LogLevel level);
void log_set_file(const std::string& path);  // empty = console only
void log_write(LogLevel level, const std::string& message);
LogLevel log_level();

// Receives every line that passes the level filter (the server window shows them live).
// Called from many threads; must be fast and must not log itself. Pass {} to remove.
using LogListener = std::function<void(LogLevel level, const std::string& line)>;
void log_set_listener(LogListener listener);

inline void log_debug(const std::string& m) { log_write(LogLevel::Debug, m); }
inline void log_info(const std::string& m) { log_write(LogLevel::Info, m); }
inline void log_warn(const std::string& m) { log_write(LogLevel::Warn, m); }
inline void log_error(const std::string& m) { log_write(LogLevel::Error, m); }

}  // namespace strojanka
