#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include "core/monitor_service.h"  // Notifier
#include "core/politicians/disclosure_source.h"
#include "core/repository.h"

// Background tracker for US Congress stock-trade disclosures, from every given source (the
// Senate's and the House's sites; see senate_efd.h and house_clerk.h for their limits). Checks
// once a day for filings in a 60-day rolling window (disclosures can legally appear up to ~45
// days after the trade), stores every new trade, posts each new filing in the public channel and
// sends it to anyone who added that politician with `monitor politician:<name>`.
//
// The first successful check of a chamber only stores what it finds: announcing 60 days of
// filings at once would flood the channel. Filings that appear after that are announced.
namespace strojanka::politicians {

class PoliticianService {
public:
    // Sources are injected (not owned) so tests can supply fakes, the same way MonitorService
    // takes a MarketDataProvider. One failing source (the Senate's site blocks many networks)
    // doesn't stop the others.
    PoliticianService(Repository& repo, std::vector<DisclosureSource*> sources, const Config& config);
    ~PoliticianService();

    void set_announcer(std::function<void(const std::string&)> announcer);  // the public channel
    void set_notifier(Notifier* notifier);  // personal DMs to a politician's watchers

    void start();  // hourly background loop, see class comment for cadence
    void stop();
    bool busy() const { return running_; }

    // Fetches new filings now, stores new trades, announces/notifies. Safe to call directly
    // (e.g. from the server console's `politicians run`). Returns how many trades were new.
    size_t tick();

private:
    size_t check(DisclosureSource& source);
    void announce_filing(const std::vector<PoliticianTradeRecord>& trades);

    Repository& repo_;
    std::vector<DisclosureSource*> sources_;
    Config config_;
    std::function<void(const std::string&)> announcer_;
    Notifier* notifier_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex loop_mutex_;
    std::condition_variable loop_cv_;
    bool stopping_ = false;
};

// One message for one filing: who, when, and its trades (at most `max_lines`, then "and N more").
std::string format_filing_announcement(const std::vector<PoliticianTradeRecord>& trades, size_t max_lines = 10);

}  // namespace strojanka::politicians
