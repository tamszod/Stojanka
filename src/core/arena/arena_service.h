#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include "core/arena/engine.h"
#include "core/market/market_data.h"
#include "core/ml/pythia.h"
#include "core/repository.h"

namespace strojanka::arena {

// What the server window and the commands show about one bot.
struct BotView {
    std::string name, origin, idea, stage, stage_label;
    double value = kStartingCash;   // live portfolio value
    double live_return = 0;         // since its current $1,000 started
    double vs_atlas = 0;            // live_return minus Atlas over the same days
    double live_drawdown = 0;
    int live_days = 0;              // trading days in the current stage
    int live_trades = 0;
    int positions = 0;
    bool backtest_passed = false;
    std::string verdict;
    double bt_return = 0, bt_cagr = 0, bt_sharpe = 0, bt_drawdown = 0;
    int bt_trades = 0;
};

// Runs the arena: every bot first has to pass the historical test, then prove itself with live
// prices in private for kProvingDays trading days; only then does it trade "publicly" and its
// trades are announced in the Discord channel. Bots that slip are sent back to prove themselves.
class ArenaService {
public:
    static constexpr int kProvingDays = 40;          // live trading days before going public
    static constexpr double kProvingMaxDrawdown = 0.15;
    static constexpr double kProvingTolerance = 0.02;  // may trail Atlas by at most 2 points
    static constexpr double kPublicMaxDrawdown = 0.25;
    static constexpr int kPublicReviewDays = 60;
    static constexpr double kPublicMaxLag = 0.10;      // demoted if 10 points behind Atlas over 60 days

    ArenaService(Repository& repo, MarketDataProvider& provider, const Config& config);
    ~ArenaService();

    // The AI model trained with the weekly tests; it feeds the Pythia and Sibylla bots.
    void set_pythia(ml::Pythia* pythia) { pythia_ = pythia; }

    // Receives messages meant for the public Discord channel.
    void set_announcer(std::function<void(const std::string&)> announcer);

    void start();  // hourly background loop
    void stop();
    bool busy() const { return busy_; }

    // One pass: refresh prices (when `refresh` is true), re-test weekly, trade every trading day up
    // to and including `last_complete` ("YYYY-MM-DD"), promote/demote, announce.
    void tick(const std::string& last_complete, bool refresh = true);

    // The latest US trading date whose prices are final at `unix_time` (the market closes at
    // 20:00 or 21:00 UTC depending on daylight saving, so a day counts from 21:30 UTC).
    static std::string last_complete_date(int64_t unix_time);

    std::vector<BotView> view();
    std::string leaderboard(bool include_private);
    std::string details(const std::string& name);
    std::string channel_id();
    void set_channel_id(const std::string& id);

private:
    void refresh_prices(bool force_full);
    bool rebuild_universe();
    void ensure_bots(const std::string& day);
    bool backtests_due();
    void run_backtests(const std::string& day);
    void advance(ArenaBotRecord& bot, const Spec& spec, int to_day);
    void judge(ArenaBotRecord& bot, const std::string& day);
    void announce(const std::string& message);
    BotView view_of(const ArenaBotRecord& bot);

    Repository& repo_;
    MarketDataProvider& provider_;
    Config config_;
    std::function<void(const std::string&)> announcer_;
    ml::Pythia* pythia_ = nullptr;
    std::mutex tick_mutex_;
    std::mutex view_mutex_;
    Universe universe_;
    std::map<std::string, int64_t> fetched_at_;
    std::atomic<bool> busy_{false};
    std::thread thread_;
    std::mutex loop_mutex_;
    std::condition_variable loop_cv_;
    bool stopping_ = false;
};

}  // namespace strojanka::arena
