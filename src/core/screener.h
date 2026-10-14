#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include "core/market/market_data.h"
#include "core/news/news_provider.h"
#include "core/repository.h"

namespace strojanka {

// Finds the stocks whose PEAK/SUPPORT calls have been reliable. Each stock in the universe gets
// the same honest walk-forward test as /report; results go to the screen_results table and are
// served by the `suggest` command. Screening ~70 stocks takes a minute or two, so it runs in the
// background once a day instead of while a user waits.
class Screener {
public:
    // With a news provider, each run also stores headlines for every screened stock, building the
    // history needed to test the value of news later.
    Screener(Repository& repo, MarketDataProvider& provider, const Config& config, news::NewsProvider* news = nullptr);
    ~Screener();

    void start();          // background loop: screens when results are missing or older than 20 hours
    void stop();
    void request_run();    // ask the background loop to screen now (no-op if already running)
    bool running() const { return running_; }

    // Screens the whole universe and stores the results. Blocks until done. Returns stocks screened.
    size_t run_now(const std::function<void(const std::string&)>& progress = {});

    std::vector<std::string> universe() const;
    static const std::vector<std::string>& default_universe();
    // screener_symbols from the settings, or the default list (also used by the arena).
    static std::vector<std::string> universe_for(const Config& config);

    // Lower bound of the hit rate's 90% Wilson interval minus the baseline; -1 with fewer than 8 calls.
    static double conservative_edge(int correct, int signals, double baseline);

    std::optional<ScreenResult> screen_one(const std::string& symbol, std::string* error);

private:
    Repository& repo_;
    MarketDataProvider& provider_;
    Config config_;
    news::NewsProvider* news_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};  // set by stop(): a run in progress ends after its current stocks
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    bool requested_ = false;
};

}  // namespace strojanka
