#pragma once

#include <string>
#include <vector>

#include "core/politicians/disclosure_source.h"

// US House of Representatives stock-trade disclosures (Periodic Transaction Reports), from the
// Clerk's official site disclosures-clerk.house.gov, which unlike the Senate's has no bot
// protection:
// - a yearly index, public_disc/financial-pdfs/<year>FD.zip, holding <year>FD.txt (tab-separated:
//   Prefix, Last, First, Suffix, FilingType, StateDst, Year, FilingDate, DocID; Windows-1252 text);
//   FilingType "P" is a trade report;
// - each report as a PDF at public_disc/ptr-pdfs/<year>/<DocID>.pdf. Electronic filings (DocID
//   starting with 2) are generated PDFs whose text can be read; the rest are scanned paper, skipped.
namespace strojanka::politicians {

class HouseClerkClient : public DisclosureSource {
public:
    std::string chamber() const override { return "House"; }
    bool start(std::string* error) override;
    std::vector<FilingRef> list_filings(const std::string& start_date, const std::string& end_date, std::string* error) override;
    std::vector<DisclosedTrade> fetch_filing(const FilingRef& ref, std::string* error) override;
};

// Exposed for unit tests. The trade reports in a <year>FD.txt index filed in [start, end].
std::vector<FilingRef> parse_house_index(const std::string& txt, const std::string& start_date, const std::string& end_date);

// Exposed for unit tests. The transactions in one electronic trade-report PDF.
std::vector<DisclosedTrade> parse_house_ptr(const std::string& pdf, const std::string& report_id, std::string* error);

}  // namespace strojanka::politicians
