#include "core/monitor_service.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <vector>

#include "common/strings.h"
#include "core/analysis/indicators.h"
#include "core/analysis/walk_forward.h"
#include "core/log.h"
#include "core/messages.h"
#include "core/portfolio.h"
#include "core/markets.h"
#include "core/thread_pool.h"

namespace strojanka {

namespace {

constexpr int kAnalysisBars = 520;                 // about two years of trading days
constexpr int kBacktestBars = 2600;                // about ten years, for the walk-forward accuracy test
constexpr int64_t kFullRefreshSeconds = 20 * 3600;  // re-download full history (adjusted prices) daily
constexpr int64_t kAfterCloseSeconds = 10 * 60;     // one check this long after the close gets the final prices
constexpr int64_t kRecalibrateSeconds = 20 * 3600;
constexpr int64_t kStaleBarSeconds = 5 * 86400;     // no alerts on data older than this
constexpr int64_t kSignalCooldownSeconds = 3 * 86400;
constexpr int64_t kNewsRefreshSeconds = 3600;
constexpr const char* kNewsWeightKey = "news_weight";
constexpr const char* kNewsSamplesKey = "news_weight_samples";

bool older_than(const std::string& iso, int64_t seconds) {
    return iso.empty() || unix_now() - iso_to_unix(iso) > seconds;
}

}  // namespace

MonitorService::MonitorService(Repository& repo, MarketDataProvider& provider, const Config& config,
                               news::NewsProvider* news)
    : repo_(repo), provider_(provider), config_(config), news_(news) {}

MonitorService::~MonitorService() { stop(); }

void MonitorService::start() {
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] {
        log_info("monitor started, checking every " + std::to_string(config_.poll_interval_minutes) + " minutes");
        for (;;) {
            try {
                run_cycle();
            } catch (const std::exception& e) {
                log_error(std::string("monitor cycle failed: ") + e.what());
            } catch (...) {
                log_error("monitor cycle failed with an unknown error");
            }
            std::unique_lock<std::mutex> lock(loop_mutex_);
            if (loop_cv_.wait_for(lock, std::chrono::minutes(config_.poll_interval_minutes), [this] { return stopping_; })) {
                break;
            }
        }
        log_info("monitor stopped");
    });
}

void MonitorService::stop() {
    {
        std::lock_guard<std::mutex> lock(loop_mutex_);
        stopping_ = true;
    }
    loop_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void MonitorService::run_cycle(bool force) {
    // A closed market has nothing new once its closing prices are in: skip those stocks until it opens.
    const int64_t now = unix_now();
    std::vector<std::string> symbols;
    size_t closed = 0;
    for (const auto& symbol : repo_.watched_symbols()) {
        const Market* market = market_for_symbol(symbol);
        if (!force && market && !market_open(*market, now)) {
            const auto stock = repo_.get_stock(symbol);
            if (stock && !stock->last_price_at.empty() && iso_to_unix(stock->last_price_at) >= last_close(*market, now) + kAfterCloseSeconds) {
                ++closed;
                continue;
            }
        }
        symbols.push_back(symbol);
    }
    if (symbols.empty()) {
        if (closed) log_debug("cycle skipped: the markets of all " + std::to_string(closed) + " watched stocks are closed");
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    ThreadPool pool(std::min<size_t>(static_cast<size_t>(config_.max_parallel_fetches), symbols.size()));
    std::vector<std::future<std::string>> jobs;  // empty string = success, otherwise the error
    for (const auto& symbol : symbols) {
        jobs.push_back(pool.submit([this, symbol]() -> std::string {
            try {
                std::string error;
                if (refresh(symbol, true, &error)) return {};
                log_warn(symbol + ": " + error);
                return symbol + ": " + error;
            } catch (const std::exception& e) {
                log_error(symbol + ": " + e.what());
                return symbol + ": " + e.what();
            }
        }));
    }
    size_t ok = 0;
    std::string last_error;
    for (auto& j : jobs) {
        const std::string error = j.get();
        if (error.empty()) ++ok;
        else last_error = error;
    }
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.finished_at = utc_now_iso();
        stats_.refreshed = ok;
        stats_.total = symbols.size();
        stats_.failed_cycles_in_a_row = ok == 0 ? stats_.failed_cycles_in_a_row + 1 : 0;
        if (!last_error.empty()) stats_.last_error = last_error;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    log_info("cycle done: " + std::to_string(ok) + "/" + std::to_string(symbols.size()) + " symbols refreshed in " +
             std::to_string(ms.count()) + " ms" + (closed ? ", " + std::to_string(closed) + " skipped (market closed)" : ""));
    learn_news_weight();
}

std::vector<SymbolMatch> MonitorService::probe_listings(const std::string& base) {
    static const std::pair<const char*, const char*> kExchanges[] = {
        {".L", "London"}, {".DE", "Xetra"}, {".AS", "Amsterdam"}, {".PA", "Paris"}, {".MI", "Milan"},
        {".SW", "Swiss"}, {".BD", "Budapest"}, {"", "US"}};
    std::vector<std::future<std::optional<SymbolMatch>>> jobs;
    for (const auto& [suffix, exchange] : kExchanges) {
        jobs.push_back(std::async(std::launch::async, [this, symbol = base + suffix, exchange]() -> std::optional<SymbolMatch> {
            std::string error;
            auto data = provider_.fetch(symbol, HistoryRange::Month, &error);
            if (!data || data->bars.empty()) return std::nullopt;
            return SymbolMatch{symbol, data->name, data->is_fund() ? "ETF" : "EQUITY", exchange};
        }));
    }
    std::vector<SymbolMatch> found;
    for (auto& j : jobs) {
        if (auto m = j.get()) found.push_back(std::move(*m));
    }
    return found;
}

CycleStats MonitorService::last_cycle() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

std::mutex& MonitorService::symbol_mutex(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(symbols_mutex_);
    auto& m = symbol_mutexes_[symbol];
    if (!m) m = std::make_unique<std::mutex>();
    return *m;
}

std::optional<StockReport> MonitorService::refresh(const std::string& symbol, bool emit_signals, std::string* error) {
    using namespace analysis;
    std::lock_guard<std::mutex> symbol_lock(symbol_mutex(symbol));

    StockRecord stock;
    if (auto existing = repo_.get_stock(symbol)) stock = *existing;
    stock.symbol = symbol;
    // Stored by an older version in pence (GBp) etc.: prices now arrive in pounds, so start over.
    const bool rescale = major_currency(stock.currency).second != 1.0;
    const bool full = rescale || older_than(stock.history_full_refresh_at, kFullRefreshSeconds);

    std::string fetch_error;
    auto data = provider_.fetch(symbol, full ? HistoryRange::TenYears : HistoryRange::Month, &fetch_error);
    if (!data) {
        if (error) *error = fetch_error;
        return std::nullopt;
    }
    if (rescale) {
        log_info(symbol + ": prices were stored in " + stock.currency + ", reloading the history in " + data->currency);
        repo_.replace_bars(symbol, data->bars);
        for (const auto& sig : repo_.pending_signals(symbol)) repo_.resolve_signal(sig.id, SignalOutcome::Expired);
    } else {
        repo_.upsert_bars(symbol, data->bars);
    }
    if (full) stock.history_full_refresh_at = utc_now_iso();
    if (!data->name.empty()) stock.name = data->name;
    if (!data->currency.empty()) stock.currency = data->currency;
    if (!data->exchange.empty()) stock.exchange = data->exchange;

    const auto bars = repo_.load_bars(symbol, kAnalysisBars);
    if (bars.size() < 30) {
        if (error) *error = "not enough price history for " + symbol + " (" + std::to_string(bars.size()) + " days)";
        return std::nullopt;
    }
    const auto closes = closes_of(bars);
    auto bar_index = [&bars](const std::string& date) -> std::optional<size_t> {
        auto it = std::lower_bound(bars.begin(), bars.end(), date, [](const Bar& b, const std::string& d) { return b.date < d; });
        if (it == bars.end() || it->date != date) return std::nullopt;
        return static_cast<size_t>(it - bars.begin());
    };

    // 1. Grade earlier calls now that more prices are known.
    bool peak_resolved = false, support_resolved = false;
    for (const auto& sig : repo_.pending_signals(symbol)) {
        const auto idx = bar_index(sig.bar_date);
        const SignalOutcome outcome =
            idx ? evaluate_outcome(bars, *idx, sig.price, sig.side, sig.move_pct, sig.horizon_bars) : SignalOutcome::Expired;
        if (outcome == SignalOutcome::Pending) continue;
        repo_.resolve_signal(sig.id, outcome);
        (sig.side == SignalSide::Peak ? peak_resolved : support_resolved) = true;
        log_info(symbol + ": " + to_string(sig.side) + " call from " + sig.bar_date + " graded " + to_string(outcome));
    }

    // 2. Learn: adjust for live mistakes, re-run the backtest calibration when due.
    StockParamsRecord learning;
    AnalysisParams params = default_params(bars);
    if (auto rec = repo_.get_params(symbol)) {
        learning = *rec;
        if (auto p = AnalysisParams::from_json(rec->params_json)) params = *p;
    }
    if (peak_resolved) params.peak_bias = adjust_bias(params.peak_bias, repo_.recent_resolved_signals(symbol, SignalSide::Peak, 10));
    if (support_resolved) params.support_bias = adjust_bias(params.support_bias, repo_.recent_resolved_signals(symbol, SignalSide::Support, 10));
    if (learning.calibrated_at.empty() || full || peak_resolved || support_resolved ||
        older_than(learning.calibrated_at, kRecalibrateSeconds)) {
        const CalibrationResult cal = calibrate(bars, params);
        params = cal.params;
        learning.symbol = symbol;
        learning.params_json = params.to_json();
        learning.calibration_score = cal.score;
        learning.calibrated_at = utc_now_iso();
        learning.calibration_count += 1;
        if (full || older_than(learning.backtest_at, kRecalibrateSeconds)) {
            const WalkForwardResult wf = walk_forward(repo_.load_bars(symbol, kBacktestBars), kAnalysisBars);
            learning.backtest_peak_signals = wf.peak_signals;
            learning.backtest_peak_correct = wf.peak_correct;
            learning.backtest_support_signals = wf.support_signals;
            learning.backtest_support_correct = wf.support_correct;
            learning.baseline_peak = wf.baseline_peak_days ? static_cast<double>(wf.baseline_peak_correct) / wf.baseline_peak_days : 0;
            learning.baseline_support = wf.baseline_support_days ? static_cast<double>(wf.baseline_support_correct) / wf.baseline_support_days : 0;
            learning.backtest_at = utc_now_iso();
        }
        repo_.save_params(learning);
        log_debug(symbol + ": calibrated " + learning.params_json);
    }

    // 3. Analyse the newest bar.
    const auto states = analyze_series(closes, params);
    const BarState& last = states.back();
    const StockProfile profile = classify(bars);

    // 4. News: refresh headlines at most hourly, summarise the last three days, nudge the scores.
    // Not for funds: an index ETF moves with its whole basket, and headlines naming it are mostly
    // about the fund company, which would only add noise (and could wrongly veto SUPPORT alerts).
    const bool fund = data->is_fund();
    if (!fund && news_ && older_than(stock.news_fetched_at, kNewsRefreshSeconds)) {
        std::string news_error;
        const int added = repo_.add_news(news_->fetch(symbol, stock.name, &news_error));
        if (!news_error.empty()) log_warn(symbol + ": " + news_error);
        log_debug(symbol + ": " + std::to_string(added) + " new headlines");
        stock.news_fetched_at = utc_now_iso();
    }
    const int64_t now = unix_now();
    const news::NewsSummary headlines =
        fund ? news::NewsSummary{} : news::summarize(repo_.recent_news(symbol, unix_to_iso(now - 72 * 3600), 200), now);
    const double news_weight = this->news_weight();
    const double peak_score = std::clamp(last.peak_score - news_weight * headlines.sentiment, 0.0, 1.0);
    const double support_score = std::clamp(last.support_score + news_weight * headlines.sentiment, 0.0, 1.0);

    stock.last_price = data->last_price > 0 ? data->last_price : bars.back().close;
    stock.last_price_at = utc_now_iso();
    stock.category = category_name(profile.category);
    stock.trend = trend_name(short_term_trend(closes, profile.daily_vol));
    stock.support_level = last.support_level;
    stock.resistance_level = last.resistance_level;
    stock.peak_score = last.peak_score;
    stock.support_score = last.support_score;
    stock.profile_json = profile.to_json();
    repo_.upsert_stock(stock);

    StockReport report;
    report.stock = stock;
    report.profile = profile;
    report.state = last;
    report.params = params;
    report.learning = learning;
    report.bars = static_cast<int>(bars.size());
    report.first_bar_date = bars.front().date;
    report.last_bar_date = bars.back().date;
    report.news = headlines;
    report.news_weight = news_weight;
    report.instrument_type = data->instrument_type;
    report.peak_score = peak_score;
    report.support_score = support_score;
    if (pythia_) report.pythia = pythia_->predict(symbol, bars);

    // 5. Alerts.
    const bool fresh = now - iso_to_unix(bars.back().date) <= kStaleBarSeconds;
    if (emit_signals && fresh && last.valid) {
        const std::string leg_key = bars[last.leg_start].date;
        if (last.direction > 0 && peak_score >= params.peak_cutoff()) {
            emit(symbol, report, SignalSide::Peak, peak_score, params.peak_cutoff(), leg_key, bars.back().close);
        } else if (last.direction < 0 && support_score >= params.support_cutoff()) {
            if (!headlines.red_flags.empty()) {
                log_info(symbol + ": SUPPORT alert held back because of red-flag news: " + headlines.red_flags.front());
            } else {
                emit(symbol, report, SignalSide::Support, support_score, params.support_cutoff(), leg_key, bars.back().close);
            }
        }
    }
    return report;
}

double MonitorService::news_weight() {
    const std::string stored = repo_.meta(kNewsWeightKey);
    if (stored.empty()) return news::kDefaultNewsWeight;
    try {
        return std::stod(stored);
    } catch (...) {
        return news::kDefaultNewsWeight;
    }
}

void MonitorService::learn_news_weight() {
    const auto graded = repo_.resolved_signals_with_news();
    const std::string samples = std::to_string(graded.size());
    if (repo_.meta(kNewsSamplesKey) == samples) return;  // nothing new to learn from
    const double before = news_weight();
    const double after = news::adjust_news_weight(before, graded);
    repo_.set_meta(kNewsWeightKey, format_double(after, 3));
    repo_.set_meta(kNewsSamplesKey, samples);
    if (after != before) log_info("news weight " + format_double(before, 3) + " -> " + format_double(after, 3));
}

void MonitorService::emit(const std::string& symbol, const StockReport& report, SignalSide side, double score,
                          double cutoff, const std::string& leg_key, double signal_price) {
    if (repo_.has_signal_since(symbol, side, unix_to_iso(unix_now() - kSignalCooldownSeconds))) return;
    SignalRecord sig;
    sig.symbol = symbol;
    sig.side = side;
    sig.bar_date = report.last_bar_date;
    sig.price = signal_price;
    sig.score = score;
    sig.threshold = cutoff;
    sig.leg_key = leg_key;
    sig.move_pct = report.params.move_pct;
    sig.horizon_bars = report.params.horizon_bars;
    sig.params_json = report.params.to_json();
    if (report.news.headlines > 0) sig.news_sentiment = report.news.sentiment;
    const int64_t signal_id = repo_.insert_signal_if_new(sig);
    if (signal_id == 0) return;  // already announced for this swing
    log_info(symbol + ": " + to_string(side) + " signal (score " + format_double(score, 2) + ")");

    const auto& s = report.stock;
    for (const auto& user : repo_.watchers(symbol)) {
        std::string message;
        if (side == SignalSide::Peak) {
            const Position pos = compute_position(repo_.transactions(user, symbol));
            if (!pos.has_shares()) continue;
            message = peak_alert(symbol, s.name, pos.shares, s.last_price, s.currency, s.resistance_level, score, report.learning);
        } else {
            message = support_alert(symbol, s.name, s.last_price, s.currency, s.support_level, score, report.learning);
        }
        if (!report.is_fund()) message += "\n" + news_alert_line(report.news);
        if (report.pythia.valid) message += "\n" + pythia_line(report.pythia);
        const int64_t notification_id = repo_.add_notification(signal_id, user, message);
        if (notifier_) notifier_->send(notification_id, user, message);
    }
}

}  // namespace strojanka
