#include "common/strings.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace strojanka {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string to_upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::optional<double> parse_amount(const std::string& text) {
    std::string kept;
    for (char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == ',' || c == '-' || c == '+') {
            kept.push_back(c);
        } else if (std::isspace(static_cast<unsigned char>(c)) || c == '$' || std::isalpha(static_cast<unsigned char>(c))) {
            continue;
        } else if (static_cast<unsigned char>(c) >= 0x80) {
            continue;  // currency signs such as € or £ (UTF-8 bytes)
        } else {
            return std::nullopt;
        }
    }
    if (kept.empty()) return std::nullopt;

    const auto commas = std::count(kept.begin(), kept.end(), ',');
    const auto dots = std::count(kept.begin(), kept.end(), '.');
    if (commas == 1 && dots > 0 && kept.rfind(',') > kept.rfind('.')) {
        // "1.234,50": dots group thousands, the comma is the decimal point (most of Europe).
        kept.erase(std::remove(kept.begin(), kept.end(), '.'), kept.end());
        kept[kept.find(',')] = '.';
    } else if (commas == 1 && dots == 0) {
        // "12,5" or "0,858291457" is a decimal comma; only "1,234" (three digits after, not "0,") groups thousands.
        const auto pos = kept.find(',');
        const std::string before = kept.substr(0, pos);
        const bool thousands = kept.size() - pos - 1 == 3 && before != "0" && before != "-0" && before != "+0" &&
                               !before.empty() && before != "-" && before != "+";
        if (!thousands) kept[pos] = '.';
    }
    kept.erase(std::remove(kept.begin(), kept.end(), ','), kept.end());
    if (std::count(kept.begin(), kept.end(), '.') > 1) return std::nullopt;

    char* end = nullptr;
    const double v = std::strtod(kept.c_str(), &end);
    if (end == kept.c_str() || *end != '\0' || !std::isfinite(v)) return std::nullopt;
    return v;
}

std::string currency_symbol(const std::string& currency) {
    if (currency.empty() || currency == "USD") return "$";
    if (currency == "EUR") return "\xE2\x82\xAC";
    if (currency == "GBP" || currency == "GBp") return "\xC2\xA3";
    if (currency == "JPY") return "\xC2\xA5";
    return currency + " ";
}

std::string format_double(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

static std::string group_thousands(const std::string& digits) {
    std::string out;
    const auto dot = digits.find('.');
    std::string int_part = digits.substr(0, dot);
    const std::string frac = dot == std::string::npos ? "" : digits.substr(dot);
    int count = 0;
    for (auto it = int_part.rbegin(); it != int_part.rend(); ++it) {
        if (count && count % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++count;
    }
    std::reverse(out.begin(), out.end());
    return out + frac;
}

std::string format_money(double v, const std::string& currency) {
    const bool neg = v < -0.004;
    const std::string body = group_thousands(format_double(std::fabs(v), 2));
    return (neg ? "-" : "") + currency_symbol(currency) + body;
}

std::string format_signed_money(double v, const std::string& currency) {
    if (v > 0.004) return "+" + format_money(v, currency);
    return format_money(v, currency);
}

std::string format_shares(double shares) {
    std::string s = format_double(shares, 9);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

std::string format_percent(double fraction, int decimals) {
    return format_double(fraction * 100.0, decimals) + "%";
}

static void civil_from_days(int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2));
}

static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

static int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

std::string unix_to_iso(int64_t ts) {
    const int64_t days = floor_div(ts, 86400);
    const int64_t secs = ts - days * 86400;
    int y;
    unsigned m, d;
    civil_from_days(days, y, m, d);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02dZ", y, m, d, static_cast<int>(secs / 3600),
                  static_cast<int>((secs % 3600) / 60), static_cast<int>(secs % 60));
    return buf;
}

std::string unix_to_date(int64_t ts) { return unix_to_iso(ts).substr(0, 10); }

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string utc_now_iso() { return unix_to_iso(unix_now()); }

int64_t iso_to_unix(const std::string& iso) {
    int y = 0, hh = 0, mm = 0, ss = 0;
    unsigned mo = 0, d = 0;
    if (std::sscanf(iso.c_str(), "%d-%u-%u", &y, &mo, &d) != 3) return 0;
    if (iso.size() >= 19) std::sscanf(iso.c_str() + 11, "%d:%d:%d", &hh, &mm, &ss);
    return days_from_civil(y, mo, d) * 86400 + hh * 3600 + mm * 60 + ss;
}

std::string normalize_symbol(const std::string& s) { return to_upper(trim(s)); }

size_t utf8_length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

std::string short_company_name(const std::string& full, size_t max_chars) {
    // Longest first, so " Corp." isn't left behind by " Corp" etc. Matched case-insensitively at the end.
    static const char* const kEndings[] = {" incorporated", " corporation", " & co.", " & co", " company", " limited",
                                           " corp.", " corp", " inc.", " inc", " co.", " plc", " ltd.", " ltd",
                                           " n.v.", " s.a.", " ag", " se", " nv", " sa"};
    std::string name = trim(full);
    for (bool changed = true; changed;) {
        changed = false;
        while (!name.empty() && (name.back() == ',' || name.back() == ' ')) name.pop_back();
        const std::string lower = to_lower(name);
        for (const char* ending : kEndings) {
            const std::string e = ending;
            if (lower.size() > e.size() + 1 && lower.compare(lower.size() - e.size(), e.size(), e) == 0) {
                name.erase(name.size() - e.size());
                changed = true;
                break;
            }
        }
    }
    if (name.size() > 4 && to_lower(name.substr(0, 4)) == "the " && name != trim(full)) name.erase(0, 4);
    if (name.empty()) name = trim(full);
    if (max_chars == 0 || utf8_length(name) <= max_chars) return name;
    std::string cut;
    size_t chars = 0;
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if ((c & 0xC0) != 0x80 && ++chars > max_chars - 1) break;
        cut.push_back(name[i]);
    }
    while (!cut.empty() && cut.back() == ' ') cut.pop_back();
    return cut + "\xE2\x80\xA6";
}

bool is_valid_symbol(const std::string& symbol) {
    if (symbol.empty() || symbol.size() > 15) return false;
    for (char c : symbol) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '^' || c == '=';
        if (!ok) return false;
    }
    return true;
}

}  // namespace strojanka
