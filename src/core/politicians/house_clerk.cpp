#include "core/politicians/house_clerk.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <regex>
#include <set>
#include <thread>
#include <tuple>

#include "common/strings.h"
#include "core/archive.h"
#include "core/http.h"
#include "core/pdf_text.h"

namespace strojanka::politicians {

namespace {

constexpr const char* kBase = "https://disclosures-clerk.house.gov/public_disc/";

// Column positions (PDF units) of the trade table, the same in every report from the Clerk's
// generator: Owner ~57, Asset ~109, Transaction type ~320, Date ~406, Notification date ~481,
// Amount ~567, Cap. gains ~671. The table starts below the header rows (y < 330).
constexpr double kTableTop = 330;
constexpr double kOwnerX = 50, kAssetX = 100, kTypeX = 315, kDateX = 400, kNotifiedX = 475, kAmountX = 560, kGainsX = 665;

// The index is Windows-1252; names like "Macías" need converting. 0x80-0x9F are mostly
// punctuation there; those few are mapped to their Unicode characters.
std::string cp1252_to_utf8(const std::string& s) {
    static const uint16_t k80[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                     0x2039, 0x0152, 0x8D,   0x017D, 0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                     0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
    std::string out;
    for (unsigned char c : s) {
        const uint32_t cp = c >= 0x80 && c < 0xA0 ? k80[c - 0x80] : c;
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::string upper(std::string s) { return to_upper(std::move(s)); }

bool all_caps(const std::string& s) {
    bool letter = false;
    for (unsigned char c : s) {
        if (std::islower(c)) return false;
        letter = letter || std::isupper(c);
    }
    return letter;
}

std::string owner_name(const std::string& code) {
    if (code.empty()) return "Self";
    if (code == "SP") return "Spouse";
    if (code == "JT") return "Joint";
    if (code == "DC") return "Dependent Child";
    return code;
}

std::string transaction_name(const std::string& code) {
    const std::string c = to_lower(trim(code));
    if (c == "p") return "Purchase";
    if (c == "s") return "Sale (Full)";
    if (c == "s (partial)") return "Sale (Partial)";
    if (c == "e") return "Exchange";
    return trim(code);
}

}  // namespace

std::vector<FilingRef> parse_house_index(const std::string& txt, const std::string& start_date, const std::string& end_date) {
    std::vector<FilingRef> out;
    for (std::string line : split(txt, '\n')) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto f = split(line, '\t');
        if (f.size() < 9 || trim(f[4]) != "P") continue;  // header row and non-trade filings
        FilingRef ref;
        ref.filed_date = normalize_date(f[7]);
        if (ref.filed_date < start_date || ref.filed_date > end_date) continue;
        ref.first_name = cp1252_to_utf8(trim(f[2]));
        ref.last_name = cp1252_to_utf8(trim(f[1]));
        if (!trim(f[3]).empty()) ref.last_name += " " + cp1252_to_utf8(trim(f[3]));
        ref.office = "Representative for " + trim(f[5]);
        ref.report_id = trim(f[8]);
        const std::string year = trim(f[6]).size() == 4 ? trim(f[6]) : ref.filed_date.substr(0, 4);
        ref.url = std::string(kBase) + "ptr-pdfs/" + year + "/" + ref.report_id + ".pdf";
        ref.electronic = !ref.report_id.empty() && ref.report_id[0] == '2';
        out.push_back(std::move(ref));
    }
    return out;
}

std::vector<DisclosedTrade> parse_house_ptr(const std::string& pdf, const std::string& report_id, std::string* error) {
    std::vector<DisclosedTrade> trades;
    std::string pdf_error;
    auto frags = pdf_text_fragments(pdf, &pdf_error);
    if (!pdf_error.empty()) {
        if (error) *error = "House report " + report_id + ": " + pdf_error;
        return trades;
    }
    std::sort(frags.begin(), frags.end(), [](const PdfText& a, const PdfText& b) { return std::tie(a.y, a.x, a.text) < std::tie(b.y, b.x, b.text); });
    frags.erase(std::unique(frags.begin(), frags.end(),
                            [](const PdfText& a, const PdfText& b) { return a.y == b.y && a.x == b.x && a.text == b.text; }),
                frags.end());
    const bool has_table = std::any_of(frags.begin(), frags.end(), [](const PdfText& f) { return trim(f.text) == "TRANSACTIONS"; });
    if (!has_table) {
        if (error) *error = "House report " + report_id + " has no transaction table where expected (its layout may have changed)";
        return trades;
    }

    std::vector<PdfText> table;
    for (const auto& f : frags) {
        if (f.y <= kTableTop) continue;
        const std::string t = trim(f.text);
        if (starts_with(t, "* For the complete") || (f.x < 10 && all_caps(t))) break;  // end of the table
        table.push_back(f);
    }

    static const std::regex kDate(R"(^\d\d/\d\d/\d{4}$)");
    std::vector<double> starts;
    for (const auto& f : table) {
        if (f.x >= kDateX && f.x < kNotifiedX && std::regex_match(trim(f.text), kDate) &&
            (starts.empty() || starts.back() != f.y)) {
            starts.push_back(f.y);
        }
    }

    static const char* const kLabels[] = {"FILING STATUS", "SUBHOLDING OF", "DESCRIPTION", "COMMENTS", "LOCATION"};
    static const std::regex kTicker(R"(\(([A-Z][A-Z0-9.\-]{0,9})\))");
    static const std::regex kAssetType(R"(\[([A-Z]{2})\]\s*$)");
    for (size_t r = 0; r < starts.size(); ++r) {
        const double y0 = starts[r] - 1, y1 = r + 1 < starts.size() ? starts[r + 1] - 1 : 1e18;
        std::vector<PdfText> block;
        for (const auto& f : table) {
            if (f.y >= y0 && f.y < y1) block.push_back(f);
        }
        // Lines under the row such as "FILING STATUS: New" or "DESCRIPTION: ..." aren't the asset name.
        std::map<double, std::pair<std::string, std::string>> labels;  // y -> (label, value)
        for (const auto& f : block) {
            if (f.x >= 115) continue;
            std::string key = trim(f.text);
            while (!key.empty() && (key.back() == ':' || key.back() == ' ')) key.pop_back();
            key = upper(key);
            for (const char* l : kLabels) {
                if (starts_with(key, l)) labels[f.y].first = l;
            }
        }
        const double first_label = labels.empty() ? 1e18 : labels.begin()->first;
        for (auto& [ly, kv] : labels) {
            for (const auto& f : block) {
                if (f.y == ly && f.x >= 115) kv.second += (kv.second.empty() ? "" : " ") + trim(f.text);
            }
        }
        auto column = [&](double lo, double hi) {
            std::string s;
            for (const auto& f : block) {
                if (f.x >= lo && f.x < hi && f.y < first_label) s += (s.empty() ? "" : " ") + trim(f.text);
            }
            return trim(s);
        };

        DisclosedTrade t;
        t.report_id = report_id;
        t.item_number = static_cast<int>(r) + 1;
        t.owner = owner_name(column(kOwnerX, kAssetX));
        std::string asset = column(kAssetX, kTypeX);
        std::smatch m;
        for (auto it = std::sregex_iterator(asset.begin(), asset.end(), kTicker); it != std::sregex_iterator(); ++it) {
            t.ticker = (*it)[1];  // the last "(TICKER)" in the name
        }
        if (std::regex_search(asset, m, kAssetType)) {
            t.asset_type = m[1];
            asset = trim(asset.substr(0, static_cast<size_t>(m.position(0))));
        }
        t.asset_name = asset;
        t.transaction_type = transaction_name(column(kTypeX, kDateX));
        t.transaction_date = normalize_date(column(kDateX, kNotifiedX));
        std::tie(t.amount_min, t.amount_max) = parse_amount_range(column(kAmountX, kGainsX));
        for (const auto& [ly, kv] : labels) {
            if ((kv.first == std::string("COMMENTS") || kv.first == std::string("DESCRIPTION")) && !kv.second.empty()) {
                t.comment += (t.comment.empty() ? "" : "; ") + kv.second;
            }
        }
        trades.push_back(std::move(t));
    }
    return trades;
}

bool HouseClerkClient::start(std::string*) { return true; }

std::vector<FilingRef> HouseClerkClient::list_filings(const std::string& start_date, const std::string& end_date, std::string* error) {
    std::vector<FilingRef> out;
    const int first_year = std::atoi(start_date.substr(0, 4).c_str()), last_year = std::atoi(end_date.substr(0, 4).c_str());
    for (int year = first_year; year <= last_year; ++year) {
        const std::string url = std::string(kBase) + "financial-pdfs/" + std::to_string(year) + "FD.zip";
        const HttpResponse res = http_get(url);
        if (!res.ok) {
            if (error) *error = "could not reach the House disclosure site: " + res.error;
            return out;
        }
        if (res.status == 404 && year == last_year && year > first_year) continue;  // early January: no index for the new year yet
        if (res.status != 200) {
            if (error) *error = "the House disclosure index for " + std::to_string(year) + " answered HTTP " + std::to_string(res.status);
            return out;
        }
        std::string zip_error;
        const auto txt = zip_entry(res.body, "FD.txt", &zip_error);
        if (!txt) {
            if (error) *error = "the House disclosure index for " + std::to_string(year) + " could not be read: " + zip_error;
            return out;
        }
        for (auto& ref : parse_house_index(*txt, start_date, end_date)) out.push_back(std::move(ref));
    }
    return out;
}

std::vector<DisclosedTrade> HouseClerkClient::fetch_filing(const FilingRef& ref, std::string* error) {
    if (!ref.electronic || ref.url.empty()) return {};
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // one report at a time, politely
    const HttpResponse res = http_get(ref.url);
    if (!res.ok || res.status != 200) {
        if (error) *error = "could not download House report " + ref.report_id + " (" + (res.ok ? "HTTP " + std::to_string(res.status) : res.error) + ")";
        return {};
    }
    return parse_house_ptr(res.body, ref.report_id, error);
}

}  // namespace strojanka::politicians
