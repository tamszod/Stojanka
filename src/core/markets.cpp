#include "core/markets.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace strojanka {

namespace {

constexpr int64_t kDay = 86400;

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int weekday(int64_t days) { return static_cast<int>(((days % 7) + 11) % 7); }  // 0 = Sunday (1970-01-01 was a Thursday)

int year_of(int64_t unix_time) {
    int64_t z = unix_time / kDay + 719468;
    if (unix_time < 0 && unix_time % kDay) --z;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    return static_cast<int>(yoe + era * 400 + (m <= 2));
}

// The n-th (1-based) Sunday of a month, or the last one with n = 0, as days since the epoch.
int64_t sunday(int year, int month, int n) {
    if (n == 0) {
        const int64_t last = days_from_civil(month == 12 ? year + 1 : year, month == 12 ? 1 : month + 1, 1) - 1;
        return last - weekday(last);
    }
    const int64_t first = days_from_civil(year, month, 1);
    return first + (7 - weekday(first)) % 7 + 7 * (n - 1);
}

bool daylight_saving(const Market& m, int64_t t) {
    const int y = year_of(t);
    const int64_t std_off = static_cast<int64_t>(m.standard_offset_minutes) * 60;
    switch (m.dst) {
        case DstRule::None: return false;
        case DstRule::US: {  // 2nd Sunday of March 02:00 local standard time -> 1st Sunday of November 02:00 local daylight time
            const int64_t start = sunday(y, 3, 2) * kDay + 2 * 3600 - std_off;
            const int64_t end = sunday(y, 11, 1) * kDay + 2 * 3600 - (std_off + 3600);
            return t >= start && t < end;
        }
        case DstRule::EU: {  // last Sunday of March -> last Sunday of October, both at 01:00 UTC
            const int64_t start = sunday(y, 3, 0) * kDay + 3600;
            const int64_t end = sunday(y, 10, 0) * kDay + 3600;
            return t >= start && t < end;
        }
        case DstRule::Sydney: {  // 1st Sunday of October 02:00 standard -> 1st Sunday of April 03:00 daylight
            const int64_t start = sunday(y, 10, 1) * kDay + 2 * 3600 - std_off;
            const int64_t end = sunday(y, 4, 1) * kDay + 3 * 3600 - (std_off + 3600);
            return t >= start || t < end;
        }
    }
    return false;
}

// Every session start/end within a few days around `t`, in order: {time, is_open_boundary}.
std::vector<std::pair<int64_t, bool>> boundaries(const Market& m, int64_t t) {
    std::vector<std::pair<int64_t, bool>> out;
    const int64_t local_day = (t + utc_offset_minutes(m, t) * 60LL) / kDay;
    for (int64_t day = local_day - 8; day <= local_day + 8; ++day) {
        const int wd = weekday(day);
        if (wd == 0 || wd == 6) continue;
        for (const auto& s : m.sessions) {
            for (const auto [minute, is_open] : {std::pair{s.open_minute, true}, std::pair{s.close_minute, false}}) {
                const int64_t guess = day * kDay + minute * 60LL;
                const int64_t at = guess - utc_offset_minutes(m, guess - m.standard_offset_minutes * 60LL) * 60LL;
                out.emplace_back(at, is_open);
            }
        }
    }
    return out;
}

}  // namespace

const std::vector<Market>& markets() {
    static const std::vector<Market> list = {
        {"US", "", "AAPL", -300, DstRule::US, {{570, 960}}},
        {"Toronto", ".TO", "RY.TO", -300, DstRule::US, {{570, 960}}},
        // Mexico's exchange keeps the New York session (07:30 local in US summer, 08:30 in winter).
        {"Mexico", ".MX", "AMXB.MX", -300, DstRule::US, {{570, 960}}},
        {"Sao Paulo", ".SA", "PETR4.SA", -180, DstRule::None, {{600, 1020}}},
        {"London", ".L", "BARC.L", 0, DstRule::EU, {{480, 990}}},
        {"Dublin", ".IR", "RYA.IR", 0, DstRule::EU, {{480, 990}}},
        {"Lisbon", ".LS", "EDP.LS", 0, DstRule::EU, {{480, 990}}},
        {"Xetra", ".DE", "SAP.DE", 60, DstRule::EU, {{540, 1050}}},
        {"Frankfurt", ".F", "SAP.F", 60, DstRule::EU, {{480, 1320}}},
        {"Amsterdam", ".AS", "ASML.AS", 60, DstRule::EU, {{540, 1050}}},
        {"Paris", ".PA", "MC.PA", 60, DstRule::EU, {{540, 1050}}},
        {"Brussels", ".BR", "ABI.BR", 60, DstRule::EU, {{540, 1050}}},
        {"Milan", ".MI", "ENI.MI", 60, DstRule::EU, {{540, 1050}}},
        {"Madrid", ".MC", "SAN.MC", 60, DstRule::EU, {{540, 1050}}},
        {"Zurich", ".SW", "NESN.SW", 60, DstRule::EU, {{540, 1050}}},
        {"Vienna", ".VI", "OMV.VI", 60, DstRule::EU, {{540, 1050}}},
        {"Stockholm", ".ST", "ERIC-B.ST", 60, DstRule::EU, {{540, 1050}}},
        {"Copenhagen", ".CO", "NOVO-B.CO", 60, DstRule::EU, {{540, 1020}}},
        {"Helsinki", ".HE", "NOKIA.HE", 120, DstRule::EU, {{600, 1110}}},
        {"Oslo", ".OL", "EQNR.OL", 60, DstRule::EU, {{540, 980}}},
        {"Warsaw", ".WA", "PKO.WA", 60, DstRule::EU, {{540, 1025}}},
        {"Prague", ".PR", "CEZ.PR", 60, DstRule::EU, {{540, 990}}},
        {"Budapest", ".BD", "OTP.BD", 60, DstRule::EU, {{540, 1020}}},
        {"Tokyo", ".T", "7203.T", 540, DstRule::None, {{540, 690}, {750, 930}}},
        {"Hong Kong", ".HK", "0700.HK", 480, DstRule::None, {{570, 720}, {780, 960}}},
        {"Shanghai", ".SS", "600519.SS", 480, DstRule::None, {{570, 690}, {780, 900}}},
        {"Shenzhen", ".SZ", "000001.SZ", 480, DstRule::None, {{570, 690}, {780, 900}}},
        {"Seoul", ".KS", "005930.KS", 540, DstRule::None, {{540, 930}}},
        {"Singapore", ".SI", "D05.SI", 480, DstRule::None, {{540, 720}, {780, 1020}}},
        {"India NSE", ".NS", "RELIANCE.NS", 330, DstRule::None, {{555, 930}}},
        {"India BSE", ".BO", "RELIANCE.BO", 330, DstRule::None, {{555, 930}}},
        {"Sydney", ".AX", "BHP.AX", 600, DstRule::Sydney, {{600, 960}}},
    };
    return list;
}

const Market* market_for_symbol(const std::string& symbol) {
    if (symbol.find('=') != std::string::npos) return nullptr;  // currencies, futures
    const auto dash = symbol.rfind('-');
    if (dash != std::string::npos && symbol.substr(dash) == "-USD") return nullptr;  // crypto, around the clock
    const auto dot = symbol.rfind('.');
    const std::string ending = dot == std::string::npos ? "" : symbol.substr(dot);  // Yahoo writes US share classes as BRK-B
    for (const auto& m : markets()) {
        if (m.suffix == ending) return &m;
    }
    return nullptr;
}

int utc_offset_minutes(const Market& market, int64_t t) { return market.standard_offset_minutes + (daylight_saving(market, t) ? 60 : 0); }

bool market_open(const Market& market, int64_t t) {
    bool open = false;
    for (const auto& [at, is_open] : boundaries(market, t)) {
        if (at > t) break;
        open = is_open;
    }
    return open;
}

int64_t next_open(const Market& market, int64_t t) {
    for (const auto& [at, is_open] : boundaries(market, t)) {
        if (is_open && at > t) return at;
    }
    return t;
}

int64_t next_close(const Market& market, int64_t t) {
    for (const auto& [at, is_open] : boundaries(market, t)) {
        if (!is_open && at > t) return at;
    }
    return t;
}

int64_t last_close(const Market& market, int64_t t) {
    int64_t last = 0;
    for (const auto& [at, is_open] : boundaries(market, t)) {
        if (at > t) break;
        if (!is_open) last = at;
    }
    return last;
}

MarketRow market_row(const Market& market, int64_t now) {
    MarketRow r;
    r.market = &market;
    r.open = market_open(market, now);
    // The trading day shown: today's while open, otherwise the next one.
    const int64_t ref = r.open ? now : next_open(market, now);
    const int64_t off = utc_offset_minutes(market, ref) * 60LL;
    const int64_t day = (ref + off) / kDay;
    const int64_t first = day * kDay + market.sessions.front().open_minute * 60LL - off;
    const int64_t last = day * kDay + market.sessions.back().close_minute * 60LL - off;
    r.hours = local_clock(first, false) + "-" + local_clock(last, false) + (market.sessions.size() > 1 ? "*" : "");
    if (r.open) {
        r.status = "OPEN until " + local_clock(next_close(market, now), false);
    } else {
        const int64_t opens = next_open(market, now);
        const bool same_day = local_clock(opens, true).substr(0, 3) == local_clock(now, true).substr(0, 3) && opens - now < kDay;
        r.status = "opens " + local_clock(opens, !same_day);
    }
    return r;
}

std::string local_clock(int64_t unix_time, bool with_weekday) {
    const std::time_t tt = static_cast<std::time_t>(unix_time);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, with_weekday ? "%a %H:%M" : "%H:%M", &tm);
    return buf;
}

std::string local_utc_label(int64_t unix_time) {
    const std::time_t tt = static_cast<std::time_t>(unix_time);
    std::tm local{}, utc{};
#if defined(_WIN32)
    localtime_s(&local, &tt);
    gmtime_s(&utc, &tt);
#else
    localtime_r(&tt, &local);
    gmtime_r(&tt, &utc);
#endif
    int minutes = (local.tm_hour - utc.tm_hour) * 60 + (local.tm_min - utc.tm_min);
    const int day_diff = local.tm_yday - utc.tm_yday;
    if (day_diff == 1 || day_diff < -1) minutes += 24 * 60;
    else if (day_diff == -1 || day_diff > 1) minutes -= 24 * 60;
    char buf[16];
    if (minutes % 60) std::snprintf(buf, sizeof buf, "UTC%+d:%02d", minutes / 60, std::abs(minutes % 60));
    else std::snprintf(buf, sizeof buf, "UTC%+d", minutes / 60);
    return buf;
}

}  // namespace strojanka
