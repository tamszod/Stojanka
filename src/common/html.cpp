#include "common/html.h"

#include <cstdlib>

#include "common/strings.h"

namespace strojanka::html {

std::string decode_entities(std::string s) {
    static const std::pair<const char*, const char*> named[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&#39;", "'"}, {"&nbsp;", " "}};
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            bool done = false;
            for (const auto& [from, to] : named) {
                if (s.compare(i, std::char_traits<char>::length(from), from) == 0) {
                    out += to;
                    i += std::char_traits<char>::length(from);
                    done = true;
                    break;
                }
            }
            if (!done && i + 2 < s.size() && s[i + 1] == '#') {
                const auto end = s.find(';', i);
                if (end != std::string::npos && end - i < 10) {
                    const bool hex = s[i + 2] == 'x' || s[i + 2] == 'X';
                    const unsigned long cp = std::strtoul(s.substr(i + (hex ? 3 : 2), end - i - (hex ? 3 : 2)).c_str(), nullptr, hex ? 16 : 10);
                    if (cp < 0x80) out.push_back(static_cast<char>(cp));
                    else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    i = end + 1;
                    done = true;
                }
            }
            if (done) continue;
        }
        out.push_back(s[i++]);
    }
    return out;
}

std::string strip_tags(const std::string& s) {
    std::string out;
    bool in_tag = false;
    for (char c : s) {
        if (c == '<') in_tag = true;
        else if (c == '>') in_tag = false;
        else if (!in_tag) out.push_back(c);
    }
    return out;
}

std::string text_of(const std::string& s) { return strojanka::trim(decode_entities(strip_tags(s))); }

}  // namespace strojanka::html
