#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "common/config.h"
#include "core/monitor_service.h"  // Notifier
#include "core/politicians/senate_efd.h"
#include "core/repository.h"

// Background tracker for US Senate stock trade disclosures (see senate_efd.h for the scraper
// and its limits -- Senate only, electronic filings only). Checks once a day for new filings in
// a 60-day rolling window (disclosures can legally appear up to ~45 days after the trade, so a
// same-day check would usually find nothing), stores every new trade, announces it in the
// configured public channel, and personally notifies anyone who added that politician with
// `monitor politician:<name>`.
namespace strojanka::politicians {

class PoliticianService {
public:
    // `source` is injected (not owned) so tests can supply a fake instead of the real scraper,
    // the same way MonitorService takes a MarketDataProvider.
    PoliticianService(Repository& repo, SenateSource& source, const Config& config);
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
    Repository& repo_;
    SenateSource& source_;
    Config config_;
    std::function<void(const std::string&)> announcer_;
    Notifier* notifier_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex loop_mutex_;
    std::condition_variable loop_cv_;
    bool stopping_ = false;
};

}  // namespace strojanka::politicians
