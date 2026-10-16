#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

// What every source of US Congress stock-trade disclosures (Periodic Transaction Reports, required
// by the STOCK Act) has in common, so PoliticianService can read the Senate and the House alike
// and tests can drive it with a fake.
namespace strojanka::politicians {

// A single disclosed transaction, scoped to one filing (see FilingRef for who filed it and
// when): the filer's name/office/filed-date live on the FilingRef once, not on every trade row.
struct DisclosedTrade {
    std::string report_id;         // the filing's id at its source
    int item_number = 0;           // row position within the filing (1-based)
    std::string transaction_date;  // ISO YYYY-MM-DD when parseable, otherwise the raw text
    std::string owner;             // "Self", "Spouse", "Joint", "Dependent Child"...
    std::string ticker;
    std::string asset_name;
    std::string asset_type;
    std::string transaction_type;  // "Purchase", "Sale (Full)", "Sale (Partial)", "Exchange"...
    double amount_min = 0;
    std::optional<double> amount_max;  // empty for an open-ended range ("Over $50,000,000")
    std::string comment;
};

// One filing in a source's list: who filed, when, and where the filing itself is.
struct FilingRef {
    std::string first_name, last_name, office, filed_date;  // filed_date: YYYY-MM-DD
    std::string report_id;
    std::string url;
    bool electronic = false;  // false = a scanned "paper" filing (unsupported, skipped)
};

class DisclosureSource {
public:
    virtual ~DisclosureSource() = default;

    virtual std::string chamber() const = 0;  // "Senate" or "House", stored with every trade

    // Must succeed before list_filings/fetch_filing are called (sessions, terms pages). On
    // failure, *error explains why in plain language.
    virtual bool start(std::string* error) = 0;

    // Every trade-report filing submitted in [start_date, end_date] (each "YYYY-MM-DD").
    virtual std::vector<FilingRef> list_filings(const std::string& start_date, const std::string& end_date, std::string* error) = 0;

    // Downloads and parses one electronic filing. Returns {} without error for a paper filing.
    virtual std::vector<DisclosedTrade> fetch_filing(const FilingRef& ref, std::string* error) = 0;
};

// "MM/DD/YYYY", "M/D/YYYY" or "YYYY-MM-DD" -> "YYYY-MM-DD"; anything else is returned trimmed but
// unchanged, so a format surprise shows up as odd-looking data rather than silently vanishing.
std::string normalize_date(const std::string& raw);

// "$1,001 - $15,000" -> {1001, 15000}; "Over $50,000,000" / "$50,000,000+" -> {50000000, nullopt}.
std::pair<double, std::optional<double>> parse_amount_range(const std::string& text);

}  // namespace strojanka::politicians
