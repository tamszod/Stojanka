#pragma once

#include <string>
#include <vector>

#include "core/models.h"

namespace strojanka::news {

// Source of headlines. Implementations must be safe to call from several threads.
class NewsProvider {
public:
    virtual ~NewsProvider() = default;
    // Recent headlines (scored, de-duplicated). `company` (e.g. "Freeport-McMoRan Inc.") improves search.
    virtual std::vector<NewsItem> fetch(const std::string& symbol, const std::string& company, std::string* error) = 0;
};

// Yahoo Finance's per-ticker RSS feed plus a Google News search, both free and keyless.
// Keeps the last 14 days.
class RssNewsProvider : public NewsProvider {
public:
    std::vector<NewsItem> fetch(const std::string& symbol, const std::string& company, std::string* error) override;
};

// Exposed for unit tests.
struct RssItem {
    std::string title, link, published, source;
};
std::vector<RssItem> parse_rss(const std::string& xml);
int64_t parse_rfc822(const std::string& date);  // 0 if unparseable
std::string search_name(const std::string& company);  // "Freeport-McMoRan Inc." -> "Freeport-McMoRan"

}  // namespace strojanka::news
