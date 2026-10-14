#include "core/screener.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>

#include "common/strings.h"
#include "core/analysis/analyzer.h"
#include "core/analysis/calibrator.h"
#include "core/analysis/classifier.h"
#include "core/analysis/indicators.h"
#include "core/analysis/walk_forward.h"
#include "core/log.h"
#include "core/thread_pool.h"

namespace strojanka {

namespace {

constexpr int64_t kRescreenSeconds = 20 * 3600;
constexpr size_t kAnalysisBars = 520;
constexpr size_t kMinBars = 400;
constexpr int kMinCalls = 8;
constexpr const char* kLastRunKey = "screener_last_run";

}  // namespace

const std::vector<std::string>& Screener::default_universe() {
    // Large, liquid US stocks across sectors.
    static const std::vector<std::string> symbols = {
        "AAPL", "MSFT", "NVDA", "AMZN", "GOOGL", "META", "TSLA", "BRK-B", "JPM",  "V",    "MA",   "UNH",
        "JNJ",  "PG",   "KO",   "PEP",  "WMT",   "COST", "HD",   "MCD",   "NKE",  "SBUX", "DIS",  "NFLX",
        "ORCL", "CSCO", "INTC", "AMD",  "QCOM",  "TXN",  "IBM",  "CRM",   "ADBE", "PFE",  "MRK",  "ABBV",
        "LLY",  "BMY",  "CVX",  "XOM",  "COP",   "SLB",  "FCX",  "NEM",   "CAT",  "DE",   "BA",   "GE",
        "HON",  "MMM",  "UPS",  "F",    "GM",    "T",    "VZ",   "BAC",   "C",    "WFC",  "GS",   "MS",
        "KMI",  "DUK",  "SO",   "NEE",  "MO",    "PM",   "CL",   "KMB",   "GIS",  "MU",
    };
    return symbols;
}

double Screener::conservative_edge(int correct, int signals, double baseline) {
    if (signals < kMinCalls) return -1;
    const double z = 1.645;
    const double n = signals;
    const double p = static_cast<double>(correct) / n;
    const double lower = (p + z * z / (2 * n) - z * std::sqrt(p * (1 - p) / n + z * z / (4 * n * n))) / (1 + z * z / n);
    return lower - baseline;
}

Screener::Screener(Repository& repo, MarketDataProvider& provider, const Config& config, news::NewsProvider* news)
    : repo_(repo), provider_(provider), config_(config), news_(news) {}

Screener::~Screener() { stop(); }

std::vector<std::string> Screener::universe() const { return universe_for(config_); }

std::vector<std::string> Screener::universe_for(const Config& config) {
    if (trim(config.screener_symbols).empty()) return default_universe();
    std::vector<std::string> out;
    for (const auto& s : split(config.screener_symbols, ',')) {
        const std::string symbol = normalize_symbol(s);
        if (is_valid_symbol(symbol) && std::find(out.begin(), out.end(), symbol) == out.end()) out.push_back(symbol);
    }
    return out;
}

std::optional<ScreenResult> Screener::screen_one(const std::string& symbol, std::string* error) {
    using namespace analysis;
    const auto data = provider_.fetch(symbol, HistoryRange::TenYears, error);
    if (!data) return std::nullopt;
    if (data->bars.size() < kMinBars) {
        if (error) *error = "only " + std::to_string(data->bars.size()) + " days of history";
        return std::nullopt;
    }
    const WalkForwardResult wf = walk_forward(data->bars, kAnalysisBars);

    const std::vector<Bar> recent(data->bars.end() - static_cast<std::ptrdiff_t>(std::min(kAnalysisBars, data->bars.size())),
                                  data->bars.end());
    const AnalysisParams params = calibrate(recent, AnalysisParams{}).params;
    const BarState now = analyze_series(closes_of(recent), params).back();

    ScreenResult r;
    r.symbol = symbol;
    r.name = data->name;
    r.currency = data->currency;
    r.category = category_name(classify(recent).category);
    r.price = data->last_price;
    r.peak_signals = wf.peak_signals;
    r.peak_correct = wf.peak_correct;
    r.baseline_peak = wf.baseline_peak_days ? static_cast<double>(wf.baseline_peak_correct) / wf.baseline_peak_days : 0;
    r.support_signals = wf.support_signals;
    r.support_correct = wf.support_correct;
    r.baseline_support = wf.baseline_support_days ? static_cast<double>(wf.baseline_support_correct) / wf.baseline_support_days : 0;
    r.peak_edge = conservative_edge(r.peak_correct, r.peak_signals, r.baseline_peak);
    r.support_edge = conservative_edge(r.support_correct, r.support_signals, r.baseline_support);
    r.direction = now.valid ? now.direction : 0;
    r.peak_score = now.peak_score;
    r.peak_cutoff = params.peak_cutoff();
    r.support_score = now.support_score;
    r.support_cutoff = params.support_cutoff();
    r.screened_at = utc_now_iso();

    if (news_) {
        std::string news_error;
        repo_.add_news(news_->fetch(symbol, data->name, &news_error));
    }
    return r;
}

size_t Screener::run_now(const std::function<void(const std::string&)>& progress) {
    if (running_.exchange(true)) return 0;
    struct Reset {
        std::atomic<bool>& flag;
        ~Reset() { flag = false; }
    } reset{running_};

    const auto symbols = universe();
    const auto started = std::chrono::steady_clock::now();
    log_info("screener: checking " + std::to_string(symbols.size()) + " stocks");
    ThreadPool pool(static_cast<size_t>(std::max(1, config_.max_parallel_fetches)));
    std::vector<std::future<std::optional<ScreenResult>>> jobs;
    for (const auto& symbol : symbols) {
        jobs.push_back(pool.submit([this, symbol, &progress]() -> std::optional<ScreenResult> {
            if (cancel_) return std::nullopt;
            std::string error;
            try {
                auto r = screen_one(symbol, &error);
                if (!r) log_warn("screener: " + symbol + ": " + error);
                else if (progress) progress(symbol);
                return r;
            } catch (const std::exception& e) {
                log_warn("screener: " + symbol + ": " + e.what());
                return std::nullopt;
            }
        }));
    }
    std::vector<ScreenResult> results;
    for (auto& j : jobs) {
        if (auto r = j.get()) results.push_back(std::move(*r));
    }
    if (cancel_) {
        log_info("screener: stopped early, keeping the previous results");
        return 0;
    }
    if (!results.empty()) {
        repo_.replace_screen_results(results);
        repo_.set_meta(kLastRunKey, utc_now_iso());
    }
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
    const auto predictable = std::count_if(results.begin(), results.end(), [](const ScreenResult& r) { return r.best_edge() > 0; });
    log_info("screener: " + std::to_string(results.size()) + " stocks screened in " + std::to_string(secs) + " s, " +
             std::to_string(predictable) + " predictable");
    return results.size();
}

void Screener::start() {
    if (thread_.joinable()) return;
    stopping_ = false;
    cancel_ = false;
    thread_ = std::thread([this] {
        for (;;) {
            const std::string last = repo_.meta(kLastRunKey);
            const bool due = last.empty() || unix_now() - iso_to_unix(last) > kRescreenSeconds;
            bool requested = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                requested = requested_;
                requested_ = false;
            }
            if (due || requested) {
                try {
                    run_now();
                } catch (const std::exception& e) {
                    log_error(std::string("screener failed: ") + e.what());
                }
            }
            std::unique_lock<std::mutex> lock(mutex_);
            if (cv_.wait_for(lock, std::chrono::hours(1), [this] { return stopping_ || requested_; }) && stopping_) break;
        }
    });
}

void Screener::stop() {
    cancel_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Screener::request_run() {
    if (running_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        requested_ = true;
    }
    cv_.notify_all();
}

}  // namespace strojanka
