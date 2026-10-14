#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

// Scrapes the US Senate's public electronic financial disclosure site (efdsearch.senate.gov)
// for Periodic Transaction Reports (PTRs): the stock trades senators must disclose under the
// STOCK Act. No clean official API exists for this (researched while building this feature):
// the well-known free community trackers (HouseStockWatcher, SenateStockWatcher) have shut
// down, so this reads the government filings directly, the way several small open-source
// scrapers do.
//
// Only *electronic* filings are parsed (HTML transaction tables); older "paper" filings
// (scanned images) are skipped, since reading those would need OCR. The House of
// Representatives is not covered at all: the House only publishes filings as PDFs, which is a
// separate, larger piece of work (PDF text extraction) not attempted here.
//
// efdsearch.senate.gov sits behind Akamai bot protection that blocks many datacenter/cloud IP
// ranges outright (confirmed while building this: a request from a cloud sandbox got an
// immediate 403 "AkamaiGHost" response, before even reaching the real page). It is expected to
// work from an ordinary home or office internet connection, which is where Strojanka normally
// runs, but this could not be fully verified end-to-end before shipping. If filings stop
// appearing, check the log for a 403 first.
namespace strojanka::politicians {

// A single disclosed transaction, scoped to one filing (see FilingRef for who filed it and
// when): fetch_filing always returns these alongside the FilingRef they came from, so the
// filer's name/office/filed-date live there once, not duplicated onto every trade row.
struct SenateTrade {
    std::string report_id;         // from the filing's URL
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

// One row of the filing search results: who filed, when, and a link to the filing itself.
struct FilingRef {
    std::string first_name, last_name, office, filed_date;
    std::string report_id;
    std::string url;
    bool electronic = false;  // false = a scanned "paper" filing (unsupported, skipped)
};

// Abstract so PoliticianService can be driven by a fake in tests, the same way MonitorService
// is driven by a fake MarketDataProvider.
class SenateSource {
public:
    virtual ~SenateSource() = default;

    // Must succeed before list_filings/fetch_filing are called. Handles the site's "accept the
    // terms" step and CSRF token. On failure, *error explains why (including a plain-language
    // note when the response looks like the Akamai bot-block described above).
    virtual bool start(std::string* error) = 0;

    // Every PTR filing submitted in [start_date, end_date] (each "YYYY-MM-DD").
    virtual std::vector<FilingRef> list_filings(const std::string& start_date, const std::string& end_date, std::string* error) = 0;

    // Downloads and parses one electronic filing's transaction table. Returns {} without error
    // for a paper filing (ref.electronic == false): there is nothing to parse.
    virtual std::vector<SenateTrade> fetch_filing(const FilingRef& ref, std::string* error) = 0;
};

// One site visit: accepts the terms once, then can list and fetch any number of filings while
// reusing the same session cookies, instead of repeating the handshake for every filing.
class SenateEfdClient : public SenateSource {
public:
    SenateEfdClient();
    ~SenateEfdClient() override;

    bool start(std::string* error) override;
    std::vector<FilingRef> list_filings(const std::string& start_date, const std::string& end_date, std::string* error) override;
    std::vector<SenateTrade> fetch_filing(const FilingRef& ref, std::string* error) override;

private:
    struct Session;
    std::unique_ptr<Session> session_;
    std::string csrf_;
};

// Exposed for unit tests: parses just the transaction table out of a filing's saved HTML, with
// no network involved.
std::vector<SenateTrade> parse_transaction_table(const std::string& html, const std::string& report_id);

}  // namespace strojanka::politicians
