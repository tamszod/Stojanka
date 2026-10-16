#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/politicians/disclosure_source.h"

// Scrapes the US Senate's public electronic financial disclosure site (efdsearch.senate.gov)
// for Periodic Transaction Reports (PTRs). No clean official API exists, and the free community
// trackers (HouseStockWatcher, SenateStockWatcher) have shut down, so this reads the filings
// directly, the way several small open-source scrapers do. Only electronic filings (HTML
// transaction tables) are parsed; scanned "paper" filings are skipped.
//
// The site sits behind Akamai bot protection that answers 403 ("AkamaiGHost") to many networks.
// It refused an ordinary home connection in Hungary (issue #4) even with browser headers and
// Windows' own TLS stack, so it probably serves US connections only; nothing in the request can
// fix that. The House source (house_clerk.h) has no such block.
namespace strojanka::politicians {

// One site visit: accepts the terms once, then can list and fetch any number of filings while
// reusing the same session cookies, instead of repeating the handshake for every filing.
class SenateEfdClient : public DisclosureSource {
public:
    SenateEfdClient();
    ~SenateEfdClient() override;

    std::string chamber() const override { return "Senate"; }
    bool start(std::string* error) override;
    std::vector<FilingRef> list_filings(const std::string& start_date, const std::string& end_date, std::string* error) override;
    std::vector<DisclosedTrade> fetch_filing(const FilingRef& ref, std::string* error) override;

private:
    struct Session;
    std::unique_ptr<Session> session_;
    std::string csrf_;
};

// Exposed for unit tests: parses just the transaction table out of a filing's saved HTML, with
// no network involved.
std::vector<DisclosedTrade> parse_transaction_table(const std::string& html, const std::string& report_id);

}  // namespace strojanka::politicians
