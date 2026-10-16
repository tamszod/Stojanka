#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Stock exchanges, their ticker endings and regular trading hours, so the bot can say which
// markets are open (`/market`, the admin panel) and skip price checks while a stock's market is
// closed. Hours are each exchange's regular session as Yahoo Finance reports it (checked
// 2026-10-16 for one stock per exchange), with Lisbon and Seoul corrected to the exchanges'
// published hours. Public holidays aren't known: on one the market shows as open.
namespace strojanka {

enum class DstRule { None, US, EU, Sydney };

struct Session {
    int open_minute;   // minutes after local midnight
    int close_minute;
};

struct Market {
    std::string name;    // short display name, e.g. "London"
    std::string suffix;  // Yahoo ticker ending, "" for the US
    std::string example; // a well-known ticker there
    int standard_offset_minutes = 0;  // UTC offset outside daylight saving time
    DstRule dst = DstRule::None;
    std::vector<Session> sessions;    // Monday to Friday, local time; two when there is a lunch break
};

const std::vector<Market>& markets();

// The market a ticker trades on: by its ending (".L" London...), no ending = US. nullptr for
// tickers that trade around the clock or that the table doesn't know (crypto "-USD", currencies
// "=X", futures "=F", unknown endings): those are always checked.
const Market* market_for_symbol(const std::string& symbol);

int utc_offset_minutes(const Market& market, int64_t unix_time);
bool market_open(const Market& market, int64_t unix_time);
int64_t next_open(const Market& market, int64_t unix_time);   // the next session start after unix_time
int64_t next_close(const Market& market, int64_t unix_time);  // the next session end after unix_time
int64_t last_close(const Market& market, int64_t unix_time);  // the latest session end at or before unix_time

// One market as /market, the admin panel and the server window show it, in this computer's time.
struct MarketRow {
    const Market* market = nullptr;
    std::string hours;   // "09:00-17:30", with "*" when there's a lunch break; the current or next trading day
    bool open = false;
    std::string status;  // "OPEN until 17:30" / "opens Mon 09:00"
};
MarketRow market_row(const Market& market, int64_t now);

// "Mon 15:30" / "15:30" in this computer's own time zone (the server's), for display.
std::string local_clock(int64_t unix_time, bool with_weekday);
// "UTC+2" for this computer's time zone right now.
std::string local_utc_label(int64_t unix_time);

}  // namespace strojanka
