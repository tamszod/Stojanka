#include "core/politicians/senate_efd.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <utility>

#include <nlohmann/json.hpp>

#include "common/html.h"
#include "common/strings.h"
#include "core/http.h"

namespace strojanka::politicians {

namespace {

constexpr const char* kLandingUrl = "https://efdsearch.senate.gov/search/home/";
constexpr const char* kSearchPageUrl = "https://efdsearch.senate.gov/search/";
constexpr const char* kReportsDataUrl = "https://efdsearch.senate.gov/search/report/data/";
constexpr const char* kBaseUrl = "https://efdsearch.senate.gov";
constexpr int kBatchSize = 100;
constexpr int kMaxPages = 500;  // safety net against an unbounded loop if the site ever misbehaves

using strojanka::html::text_of;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "MM/DD/YYYY" (or already "YYYY-MM-DD") -> "YYYY-MM-DD"; anything else is returned unchanged
// so a format surprise shows up as odd-looking data rather than silently vanishing.
std::string normalize_date(const std::string& raw) {
    const std::string s = trim(raw);
    if (s.size() == 10 && s[2] == '/' && s[5] == '/') return s.substr(6, 4) + "-" + s.substr(0, 2) + "-" + s.substr(3, 2);
    if (s.size() == 10 && s[4] == '-' && s[7] == '-') return s;
    return s;
}

std::string iso_to_mdy(const std::string& iso) {
    if (iso.size() != 10 || iso[4] != '-' || iso[7] != '-') return iso;
    return iso.substr(5, 2) + "/" + iso.substr(8, 2) + "/" + iso.substr(0, 4);
}

int parse_int_or(const std::string& text, int fallback) {
    std::string digits;
    for (char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c))) digits.push_back(c);
    }
    return digits.empty() ? fallback : std::atoi(digits.c_str());
}

double parse_dollars(std::string text) {
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

// "$1,001 - $15,000" -> {1001, 15000}; "Over $50,000,000" / "$50,000,000+" -> {50000000, nullopt}.
std::pair<double, std::optional<double>> parse_amount_range(const std::string& text) {
    const std::string low = lower(text);
    const bool open_ended = low.find("over") != std::string::npos || low.find("more than") != std::string::npos ||
                            low.find('+') != std::string::npos;
    const auto dash = text.find_first_of("-\xE2");  // '-' or the first byte of an en/em dash
    if (!open_ended && dash != std::string::npos) {
        return {parse_dollars(text.substr(0, dash)), parse_dollars(text.substr(dash + 1))};
    }
    const double v = parse_dollars(text);
    return {v, open_ended ? std::optional<double>() : std::optional<double>(v)};
}

// Finds every top-level (non-nested) <tag ...> ... </tag> block, case-insensitively.
std::vector<std::pair<size_t, size_t>> find_blocks(const std::string& lower_html, const std::string& tag) {
    std::vector<std::pair<size_t, size_t>> out;
    const std::string open_marker = "<" + tag;
    const std::string close_marker = "</" + tag;
    auto is_boundary = [](char c) { return c == '>' || c == '/' || std::isspace(static_cast<unsigned char>(c)); };
    size_t pos = 0;
    while (pos < lower_html.size()) {
        const size_t open_pos = lower_html.find(open_marker, pos);
        if (open_pos == std::string::npos) break;
        const size_t after_open = open_pos + open_marker.size();
        if (after_open < lower_html.size() && !is_boundary(lower_html[after_open])) {
            pos = open_pos + 1;
            continue;
        }
        size_t depth = 1, cursor = after_open;
        size_t close_pos = std::string::npos;
        while (depth > 0) {
            const size_t next_open = lower_html.find(open_marker, cursor);
            const size_t next_close = lower_html.find(close_marker, cursor);
            if (next_close == std::string::npos) break;
            if (next_open != std::string::npos && next_open < next_close &&
                (next_open + open_marker.size() >= lower_html.size() || is_boundary(lower_html[next_open + open_marker.size()]))) {
                ++depth;
                cursor = next_open + open_marker.size();
                continue;
            }
            --depth;
            cursor = next_close + close_marker.size();
            if (depth == 0) close_pos = next_close;
        }
        if (close_pos == std::string::npos) break;
        const size_t close_end = lower_html.find('>', close_pos);
        if (close_end == std::string::npos) break;
        out.emplace_back(open_pos, close_end + 1);
        pos = close_end + 1;
    }
    return out;
}

// A row's cells can be <td> or <th>; collect both, in the order they appear in the document.
std::vector<std::string> cells_of_row(const std::string& html, const std::string& lower_html, size_t row_start, size_t row_end) {
    std::vector<std::string> cells;
    struct Cell {
        size_t pos;
        std::string text;
    };
    std::vector<Cell> found;
    for (const char* tag : {"td", "th"}) {
        const std::string sub_lower = lower_html.substr(row_start, row_end - row_start);
        const std::string sub_html = html.substr(row_start, row_end - row_start);
        for (const auto& [s, e] : find_blocks(sub_lower, tag)) {
            const size_t content_start = sub_html.find('>', s) + 1;
            found.push_back({s, content_start < e ? text_of(sub_html.substr(content_start, e - content_start)) : std::string()});
        }
    }
    std::sort(found.begin(), found.end(), [](const Cell& a, const Cell& b) { return a.pos < b.pos; });
    for (auto& c : found) cells.push_back(std::move(c.text));
    return cells;
}

std::string normalize_key(const std::string& header) {
    std::string key;
    for (char c : lower(header)) {
        if (std::isalnum(static_cast<unsigned char>(c))) key.push_back(c);
    }
    return key;
}

// Maps a normalized header to the field it represents, or "" if not recognized.
std::string field_for(const std::string& key) {
    static const std::vector<std::pair<std::string, std::string>> map = {
        {"", "item_number"}, {"number", "item_number"}, {"item", "item_number"},
        {"transactiondate", "transaction_date"}, {"date", "transaction_date"},
        {"owner", "owner"}, {"ticker", "ticker"}, {"assetname", "asset_name"}, {"assettype", "asset_type"},
        {"type", "transaction_type"}, {"transactiontype", "transaction_type"}, {"amount", "amount"}, {"comment", "comment"},
    };
    for (const auto& [k, v] : map) {
        if (k == key) return v;
    }
    return {};
}

}  // namespace

std::vector<SenateTrade> parse_transaction_table(const std::string& html, const std::string& report_id) {
    std::vector<SenateTrade> out;
    const std::string lower_html = lower(html);

    // Pick the table whose header names "transaction"/"owner"/"ticker"/"amount", or failing
    // that, the first one whose first body row has at least 8 <td> cells.
    const auto tables = find_blocks(lower_html, "table");
    std::vector<std::vector<std::string>> chosen_rows;
    for (const auto& [ts, te] : tables) {
        const std::string table_lower = lower_html.substr(ts, te - ts);
        const auto rows_range = find_blocks(table_lower, "tr");
        if (rows_range.empty()) continue;
        std::vector<std::vector<std::string>> rows;
        for (const auto& [rs, re] : rows_range) rows.push_back(cells_of_row(html, lower_html, ts + rs, ts + re));
        std::string header_joined;
        for (const auto& c : rows[0]) header_joined += lower(c) + " ";
        const bool header_match = header_joined.find("transaction") != std::string::npos && header_joined.find("owner") != std::string::npos &&
                                  header_joined.find("ticker") != std::string::npos && header_joined.find("amount") != std::string::npos;
        const bool fallback_match = rows.size() > 1 && rows[1].size() >= 8;
        if (header_match) {
            chosen_rows = std::move(rows);
            break;
        }
        if (chosen_rows.empty() && fallback_match) chosen_rows = std::move(rows);
    }
    if (chosen_rows.empty()) return out;

    std::vector<std::string> headers = chosen_rows[0];
    if (headers.size() < 7) headers = {"#", "Transaction Date", "Owner", "Ticker", "Asset Name", "Asset Type", "Type", "Amount", "Comment"};
    std::vector<std::string> field_names;
    for (const auto& h : headers) field_names.push_back(field_for(normalize_key(h)));

    for (size_t r = 1; r < chosen_rows.size(); ++r) {
        const auto& cells = chosen_rows[r];
        if (cells.empty()) continue;
        std::map<std::string, std::string> fields;
        for (size_t c = 0; c < cells.size() && c < field_names.size(); ++c) {
            if (!field_names[c].empty()) fields[field_names[c]] = cells[c];
        }
        SenateTrade t;
        t.report_id = report_id;
        t.item_number = parse_int_or(fields["item_number"], static_cast<int>(r));
        t.transaction_date = normalize_date(fields["transaction_date"]);
        t.owner = fields["owner"];
        t.ticker = fields["ticker"];
        t.asset_name = fields["asset_name"];
        t.asset_type = fields["asset_type"];
        t.transaction_type = fields["transaction_type"];
        std::tie(t.amount_min, t.amount_max) = parse_amount_range(fields["amount"]);
        t.comment = fields["comment"];
        out.push_back(std::move(t));
    }
    return out;
}

// --- HTTP session (cookie-preserving, since the site needs an "accept terms" step first) -----

struct SenateEfdClient::Session {
    CURL* curl = nullptr;
    Session() {
        curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // enables the in-memory cookie engine
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36");
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        }
    }
    ~Session() {
        if (curl) curl_easy_cleanup(curl);
    }

    struct Result {
        bool ok = false;
        long status = 0;
        std::string body, error;
    };

    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
        return size * nmemb;
    }

    Result perform(const std::string& url, bool is_post, const std::string& post_body, const std::vector<std::string>& headers,
                   const std::string& referer) {
        Result r;
        if (!curl) {
            r.error = "curl init failed";
            return r;
        }
        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_REFERER, referer.empty() ? nullptr : referer.c_str());
        curl_slist* list = nullptr;
        for (const auto& h : headers) list = curl_slist_append(list, h.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
        if (is_post) {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(post_body.size()));
        } else {
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        }
        const CURLcode rc = curl_easy_perform(curl);
        if (list) curl_slist_free_all(list);
        if (rc != CURLE_OK) {
            r.error = curl_easy_strerror(rc);
            return r;
        }
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        r.ok = true;
        r.status = status;
        r.body = std::move(body);
        return r;
    }

    Result get(const std::string& url, const std::string& referer) { return perform(url, false, "", {}, referer); }
    Result post_form(const std::string& url, const std::vector<std::pair<std::string, std::string>>& fields,
                     const std::string& referer, const std::vector<std::string>& headers = {}) {
        std::string body;
        for (const auto& [k, v] : fields) {
            if (!body.empty()) body += "&";
            body += url_encode(k) + "=" + url_encode(v);
        }
        return perform(url, true, body, headers, referer);
    }

    std::string cookie(const std::string& name) {
        curl_slist* list = nullptr;
        curl_easy_getinfo(curl, CURLINFO_COOKIELIST, &list);
        std::string value;
        for (curl_slist* n = list; n; n = n->next) {
            const auto parts = split(n->data, '\t');
            if (parts.size() >= 7 && parts[5] == name) value = parts[6];
        }
        curl_slist_free_all(list);
        return value;
    }
};

namespace {
std::string extract_csrf_token(const std::string& html) {
    const std::string marker = "name=\"csrfmiddlewaretoken\"";
    const auto pos = html.find(marker);
    if (pos == std::string::npos) return {};
    const auto value_pos = html.find("value=\"", pos);
    if (value_pos == std::string::npos) return {};
    const auto start = value_pos + 7;
    const auto end = html.find('"', start);
    if (end == std::string::npos) return {};
    return html.substr(start, end - start);
}
}  // namespace

SenateEfdClient::SenateEfdClient() : session_(std::make_unique<Session>()) {}
SenateEfdClient::~SenateEfdClient() = default;

bool SenateEfdClient::start(std::string* error) {
    const auto landing = session_->get(kLandingUrl, "");
    if (!landing.ok) {
        if (error) *error = "network error reaching the Senate disclosure site: " + landing.error;
        return false;
    }
    if (landing.status == 403) {
        if (error) *error = "the Senate site refused the connection (HTTP 403) -- it blocks some cloud/datacenter networks; "
                           "this usually works from an ordinary home or office connection instead";
        return false;
    }
    if (landing.status != 200) {
        if (error) *error = "the Senate site answered HTTP " + std::to_string(landing.status);
        return false;
    }
    const std::string token = extract_csrf_token(landing.body);
    if (token.empty()) {
        if (error) *error = "could not find the agreement form on the Senate site (its page may have changed)";
        return false;
    }
    const auto agree = session_->post_form(kLandingUrl, {{"csrfmiddlewaretoken", token}, {"prohibition_agreement", "1"}}, kLandingUrl);
    if (!agree.ok || (agree.status != 200 && agree.status != 302)) {
        if (error) *error = "could not accept the Senate site's terms (HTTP " + std::to_string(agree.status) + ")";
        return false;
    }
    csrf_ = session_->cookie("csrftoken");
    if (csrf_.empty()) csrf_ = session_->cookie("csrf");
    if (csrf_.empty()) {
        if (error) *error = "the Senate site did not set the expected session cookie after accepting its terms";
        return false;
    }
    return true;
}

std::vector<FilingRef> SenateEfdClient::list_filings(const std::string& start_date, const std::string& end_date, std::string* error) {
    std::vector<FilingRef> out;
    const std::string submitted_start = iso_to_mdy(start_date) + " 00:00:00";
    const std::string submitted_end = iso_to_mdy(end_date) + " 23:59:59";
    int start = 0;
    for (int page = 0; page < kMaxPages; ++page) {
        const auto res = session_->post_form(
            kReportsDataUrl,
            {{"start", std::to_string(start)}, {"length", std::to_string(kBatchSize)}, {"report_types", "[11]"},
             {"filer_types", "[1]"}, {"submitted_start_date", submitted_start}, {"submitted_end_date", submitted_end},
             {"csrfmiddlewaretoken", csrf_}},
            kSearchPageUrl, {"X-CSRFToken: " + csrf_, "X-Requested-With: XMLHttpRequest"});
        if (!res.ok || res.status != 200) {
            if (error) *error = "the Senate filing search failed (HTTP " + std::to_string(res.status) + ")";
            return out;
        }
        const auto j = nlohmann::json::parse(res.body, nullptr, false);
        if (!j.is_object() || !j.contains("data") || !j["data"].is_array()) {
            if (error) *error = "the Senate site's search results looked unexpected (its API may have changed)";
            return out;
        }
        const auto& rows = j["data"];
        if (rows.empty()) break;
        for (const auto& row : rows) {
            if (!row.is_array() || row.size() < 4) continue;
            auto at = [&](size_t i) { return i < row.size() && row[i].is_string() ? row[i].get<std::string>() : std::string(); };
            const std::string link_html = at(3);
            const auto href_pos = link_html.find("href=\"");
            if (href_pos == std::string::npos) continue;
            const auto href_start = href_pos + 6;
            const auto href_end = link_html.find('"', href_start);
            if (href_end == std::string::npos) continue;
            std::string href = link_html.substr(href_start, href_end - href_start);
            const bool is_ptr = href.find("/search/view/ptr/") != std::string::npos;
            const bool is_paper = href.find("/search/view/paper/") != std::string::npos;
            if (!is_ptr && !is_paper) continue;

            FilingRef ref;
            ref.first_name = text_of(at(0));
            ref.last_name = text_of(at(1));
            ref.office = text_of(at(2));
            ref.filed_date = normalize_date(text_of(at(4)));
            ref.electronic = is_ptr;
            ref.url = starts_with(href, "http") ? href : std::string(kBaseUrl) + href;
            std::string path = href;
            while (!path.empty() && path.back() == '/') path.pop_back();
            const auto slash = path.find_last_of('/');
            ref.report_id = slash == std::string::npos ? path : path.substr(slash + 1);
            out.push_back(std::move(ref));
        }
        start += static_cast<int>(rows.size());
        const int records_filtered = j.value("recordsFiltered", 0);
        if (static_cast<int>(rows.size()) < kBatchSize || start >= records_filtered) break;
    }
    return out;
}

std::vector<SenateTrade> SenateEfdClient::fetch_filing(const FilingRef& ref, std::string* error) {
    std::vector<SenateTrade> out;
    if (!ref.electronic || ref.url.empty()) return out;  // paper filing: nothing we can parse
    const auto res = session_->get(ref.url, kSearchPageUrl);
    if (!res.ok || res.status != 200) {
        if (error) *error = "could not open filing " + ref.report_id + " (HTTP " + std::to_string(res.status) + ")";
        return out;
    }
    return parse_transaction_table(res.body, ref.report_id);
}

}  // namespace strojanka::politicians
