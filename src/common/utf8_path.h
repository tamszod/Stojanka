#pragma once

#include <filesystem>
#include <string>

namespace strojanka {

// All strings in Strojanka are UTF-8. These convert to and from filesystem paths without
// depending on whether char8_t exists (C++20) or not (C++17).
inline std::filesystem::path utf8_path(const std::string& s) {
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
#else
    return std::filesystem::u8path(s);
#endif
}

inline std::string path_utf8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

inline std::string path_utf8_generic(const std::filesystem::path& p) {
    const auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

}  // namespace strojanka
