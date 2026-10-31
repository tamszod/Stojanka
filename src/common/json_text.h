#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace strojanka {

// json.dump() for text going to a window or anywhere else that must keep working: plain dump()
// throws on any byte that isn't valid UTF-8, and one Windows error message in Hungarian on a
// background thread ended the whole program that way. Bad bytes become U+FFFD here instead.
inline std::string json_text(const nlohmann::json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace strojanka
