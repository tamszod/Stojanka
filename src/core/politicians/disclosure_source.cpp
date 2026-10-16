#include "core/politicians/disclosure_source.h"

#include <cctype>
#include <cstdlib>

#include "common/strings.h"

namespace strojanka::politicians {

namespace {

double parse_dollars(const std::string& text) {
    std::string digits;
    bool dot = false;
    for (char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c))) digits.push_back(c);
        else if (c == '.' && !dot) {
            digits.push_back(c);
            dot = true;
        }
    }
    return digits.empty() ? 0.0 : std::atof(digits.c_str());
}

}  // namespace

std::string normalize_date(const std::string& raw) {
    const std::string s = trim(raw);
    if (s.size() == 10 && s[4] == '-' && s[7] == '-') return s;
    const auto parts = split(s, '/');
    if (parts.size() == 3 && parts[2].size() == 4 && !parts[0].empty() && parts[0].size() <= 2 && !parts[1].empty() &&
        parts[1].size() <= 2) {
        for (const auto& p : parts) {
            for (char c : p) {
                if (!std::isdigit(static_cast<unsigned char>(c))) return s;
            }
        }
        auto two = [](const std::string& p) { return p.size() == 1 ? "0" + p : p; };
        return parts[2] + "-" + two(parts[0]) + "-" + two(parts[1]);
    }
    return s;
}

std::pair<double, std::optional<double>> parse_amount_range(const std::string& text) {
    const std::string low = to_lower(text);
    const bool open_ended = low.find("over") != std::string::npos || low.find("more than") != std::string::npos ||
                            low.find('+') != std::string::npos;
    const auto dash = text.find_first_of("-\xE2");  // '-' or the first byte of an en/em dash
    if (!open_ended && dash != std::string::npos) {
        return {parse_dollars(text.substr(0, dash)), parse_dollars(text.substr(dash + 1))};
    }
    const double v = parse_dollars(text);
    return {v, open_ended ? std::optional<double>() : std::optional<double>(v)};
}

}  // namespace strojanka::politicians
