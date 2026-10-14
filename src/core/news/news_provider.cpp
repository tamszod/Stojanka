#include "core/news/news_provider.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "common/strings.h"
#include "core/http.h"
#include "core/news/sentiment.h"

namespace strojanka::news {

namespace {

constexpr int64_t kMaxAgeSeconds = 14 * 86400;

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

// Text of the first <tag ...>...</tag> inside `block` (CDATA unwrapped, entities decoded).
std::string tag_text(const std::string& block, const std::string& tag) {
    const auto open = block.find("<" + tag);
    if (open == std::string::npos) return {};
    const auto start = block.find('>', open);
    const auto close = block.find("</" + tag + ">", start);
    if (start == std::string::npos || close == std::string::npos) return {};
    std::string text = block.substr(start + 1, close - start - 1);
    const auto cdata = text.find("<![CDATA[");
    if (cdata != std::string::npos) {
        const auto end = text.find("]]>", cdata);
        text = text.substr(cdata + 9, end == std::string::npos ? std::string::npos : end - cdata - 9);
        return trim(strip_tags(text));
    }
    return trim(decode_entities(strip_tags(decode_entities(text))));
}

bool contains_word(const std::string& haystack_lower, const std::string& word_lower) {
    size_t pos = 0;
    while ((pos = haystack_lower.find(word_lower, pos)) != std::string::npos) {
        const bool left = pos == 0 || !std::isalnum(static_cast<unsigned char>(haystack_lower[pos - 1]));
        const size_t end = pos + word_lower.size();
        const bool right = end >= haystack_lower.size() || !std::isalnum(static_cast<unsigned char>(haystack_lower[end]));
        if (left && right) return true;
        pos = end;
    }
    return false;
}

// Feeds (Yahoo's per-ticker one included) mix in unrelated market stories, so a headline is kept
// only if it names the ticker or the company.
void add_items(const std::string& symbol, const std::vector<RssItem>& raw, const std::string& name, int64_t now,
               std::set<std::string>& seen, std::vector<NewsItem>& out) {
    const std::string sym = to_lower(symbol);
    std::string first_word = to_lower(name.substr(0, name.find_first_of(" -,")));
    if (first_word.size() < 4) first_word.clear();
    for (const auto& r : raw) {
        const int64_t published = parse_rfc822(r.published);
        if (published == 0 || now - published > kMaxAgeSeconds) continue;
        std::string title = r.title;
        if (!r.source.empty() && title.size() > r.source.size() + 3 &&
            title.compare(title.size() - r.source.size() - 3, std::string::npos, " - " + r.source) == 0) {
            title = title.substr(0, title.size() - r.source.size() - 3);  // Google appends " - Publisher"
        }
        if (title.empty()) continue;
        const std::string lower = to_lower(title);
        const bool about = (sym.size() >= 2 && contains_word(lower, sym)) || (!first_word.empty() && contains_word(lower, first_word));
        if (!about) continue;
        NewsItem item;
        item.symbol = symbol;
        item.title = title;
        item.link = r.link;
        item.source = r.source;
        item.published_at = unix_to_iso(published);
        item.title_key = title_key(title);
        if (item.title_key.empty() || !seen.insert(item.title_key).second) continue;
        const HeadlineScore score = score_headline(title);
        item.sentiment = score.sentiment;
        item.red_flag = score.red_flag;
        out.push_back(std::move(item));
    }
}

}  // namespace

std::vector<RssItem> parse_rss(const std::string& xml) {
    std::vector<RssItem> items;
    size_t pos = 0;
    while ((pos = xml.find("<item", pos)) != std::string::npos) {
        const auto end = xml.find("</item>", pos);
        if (end == std::string::npos) break;
        const std::string block = xml.substr(pos, end - pos);
        RssItem item;
        item.title = tag_text(block, "title");
        item.link = tag_text(block, "link");
        item.published = tag_text(block, "pubDate");
        item.source = tag_text(block, "source");
        items.push_back(std::move(item));
        pos = end + 7;
    }
    return items;
}

int64_t parse_rfc822(const std::string& date) {
    // "Sun, 11 Oct 2026 16:03:00 +0000" or "... GMT"
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    char mon[4] = {}, zone[16] = {};
    const char* p = date.c_str();
    if (const char* comma = std::strchr(p, ',')) p = comma + 1;
    if (std::sscanf(p, " %d %3s %d %d:%d:%d %15s", &day, mon, &year, &hh, &mm, &ss, zone) < 6) return 0;
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int month = 0;
    for (int i = 0; i < 12; ++i) {
        if (std::string(mon) == months[i]) month = i + 1;
    }
    if (month == 0) return 0;
    char iso[32];
    std::snprintf(iso, sizeof iso, "%04d-%02d-%02dT%02d:%02d:%02dZ", year, month, day, hh, mm, ss);
    int64_t t = iso_to_unix(iso);
    if ((zone[0] == '+' || zone[0] == '-') && std::strlen(zone) == 5) {
        const int offset = ((zone[1] - '0') * 10 + (zone[2] - '0')) * 3600 + ((zone[3] - '0') * 10 + (zone[4] - '0')) * 60;
        t += zone[0] == '+' ? -offset : offset;
    }
    return t;
}

std::string search_name(const std::string& company) {
    std::string name = trim(company);
    static const char* suffixes[] = {", Inc.", " Inc.", " Inc", " Incorporated", " Corporation", " Corp.", " Corp",
                                     " Company", " Co.", " Ltd.", " Ltd", " plc", " PLC", " Holdings", " N.V.", " S.A.", ","};
    for (bool changed = true; changed;) {
        changed = false;
        for (const char* s : suffixes) {
            const std::string suf = s;
            if (name.size() > suf.size() && name.compare(name.size() - suf.size(), std::string::npos, suf) == 0) {
                name = trim(name.substr(0, name.size() - suf.size()));
                changed = true;
            }
        }
    }
    if (starts_with(name, "The ")) name = name.substr(4);
    return name;
}

std::vector<NewsItem> RssNewsProvider::fetch(const std::string& symbol, const std::string& company, std::string* error) {
    const int64_t now = unix_now();
    std::vector<NewsItem> out;
    std::set<std::string> seen;
    std::string errors;

    const std::string name = search_name(company);
    const HttpResponse yahoo = http_get("https://feeds.finance.yahoo.com/rss/2.0/headline?s=" + url_encode(symbol) +
                                        "&region=US&lang=en-US");
    if (yahoo.ok && yahoo.status == 200) add_items(symbol, parse_rss(yahoo.body), name, now, seen, out);
    else errors += "Yahoo news: " + (yahoo.ok ? "HTTP " + std::to_string(yahoo.status) : yahoo.error) + "; ";

    const std::string query = (name.empty() ? symbol : "\"" + name + "\"") + " stock";
    const HttpResponse google = http_get("https://news.google.com/rss/search?q=" + url_encode(query) + "&hl=en-US&gl=US&ceid=US:en");
    if (google.ok && google.status == 200) add_items(symbol, parse_rss(google.body), name, now, seen, out);
    else errors += "Google News: " + (google.ok ? "HTTP " + std::to_string(google.status) : google.error) + "; ";

    std::sort(out.begin(), out.end(), [](const NewsItem& a, const NewsItem& b) { return a.published_at > b.published_at; });
    if (error) *error = errors;
    return out;
}

}  // namespace strojanka::news
