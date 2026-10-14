#include "core/politicians/politician_service.h"

#include <sstream>

#include "common/strings.h"
#include "core/log.h"

namespace strojanka::politicians {

namespace {

constexpr int kLookbackDays = 60;
constexpr int64_t kCheckEverySeconds = 24 * 3600;
constexpr const char* kLastRunKey = "politicians_last_run";

}  // namespace

PoliticianService::PoliticianService(Repository& repo, SenateSource& source, const Config& config)
    : repo_(repo), source_(source), config_(config) {}

PoliticianService::~PoliticianService() { stop(); }

void PoliticianService::set_announcer(std::function<void(const std::string&)> announcer) { announcer_ = std::move(announcer); }
void PoliticianService::set_notifier(Notifier* notifier) { notifier_ = notifier; }

void PoliticianService::start() {
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] {
        for (;;) {
            const std::string last = repo_.meta(kLastRunKey);
            if (last.empty() || unix_now() - iso_to_unix(last) >= kCheckEverySeconds) {
                try {
                    tick();
                } catch (const std::exception& e) {
                    log_error(std::string("politicians failed: ") + e.what());
                }
            }
            std::unique_lock<std::mutex> lock(loop_mutex_);
            if (loop_cv_.wait_for(lock, std::chrono::hours(1), [this] { return stopping_; })) break;
        }
    });
}

void PoliticianService::stop() {
    {
        std::lock_guard<std::mutex> lock(loop_mutex_);
        stopping_ = true;
    }
    loop_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

size_t PoliticianService::tick() {
    if (running_.exchange(true)) return 0;
    struct Reset {
        std::atomic<bool>& flag;
        ~Reset() { flag = false; }
    } reset{running_};

    std::string error;
    if (!source_.start(&error)) {
        log_warn("politicians: " + error);
        return 0;
    }
    const std::string today = unix_to_date(unix_now());
    const std::string since = unix_to_date(unix_now() - static_cast<int64_t>(kLookbackDays) * 86400);
    auto filings = source_.list_filings(since, today, &error);
    if (!error.empty()) {
        log_warn("politicians: " + error);
        return 0;
    }
    log_info("politicians: " + std::to_string(filings.size()) + " Senate filings in the last " + std::to_string(kLookbackDays) +
             " days");

    std::vector<PoliticianTradeRecord> all_new;
    int skipped_paper = 0, fetch_errors = 0;
    for (const auto& ref : filings) {
        if (!ref.electronic) {
            ++skipped_paper;
            continue;
        }
        if (repo_.has_politician_filing(ref.report_id)) continue;  // already ingested on an earlier run
        std::string fetch_error;
        const auto trades = source_.fetch_filing(ref, &fetch_error);
        if (!fetch_error.empty()) {
            ++fetch_errors;
            log_warn("politicians: " + fetch_error);
            continue;
        }
        const std::string fetched_at = utc_now_iso();
        std::vector<PoliticianTradeRecord> records;
        for (const auto& t : trades) {
            PoliticianTradeRecord r;
            r.chamber = "Senate";
            r.first_name = ref.first_name;
            r.last_name = ref.last_name;
            r.office = ref.office;
            r.filed_date = ref.filed_date;
            r.report_id = t.report_id;
            r.item_number = t.item_number;
            r.transaction_date = t.transaction_date;
            r.owner = t.owner;
            r.ticker = t.ticker;
            r.asset_name = t.asset_name;
            r.asset_type = t.asset_type;
            r.transaction_type = t.transaction_type;
            r.amount_min = t.amount_min;
            r.amount_max = t.amount_max;
            r.comment = t.comment;
            r.fetched_at = fetched_at;
            records.push_back(std::move(r));
        }
        const auto added = repo_.add_politician_trades(records);
        all_new.insert(all_new.end(), added.begin(), added.end());
    }

    for (const auto& t : all_new) {
        const std::string name = t.full_name();
        const bool has_ticker = !t.ticker.empty() && t.ticker != "--";
        std::ostringstream m;
        m << "\xF0\x9F\x8F\x9B\xEF\xB8\x8F **" << name << "**";
        if (!t.office.empty()) m << " (" << t.office << ")";
        m << " disclosed a trade" << (t.filed_date.empty() ? "" : " filed " + t.filed_date) << ":\n"
          << "- " << (t.transaction_type.empty() ? "Transaction" : t.transaction_type) << " **"
          << (has_ticker ? t.ticker : t.asset_name) << "**";
        if (has_ticker && !t.asset_name.empty()) m << " (" << t.asset_name << ")";
        m << ", " << (t.owner.empty() ? "Self" : t.owner) << ", " << format_money(t.amount_min)
          << (t.amount_max ? " - " + format_money(*t.amount_max) : "+");
        if (!t.transaction_date.empty()) m << ", traded " << t.transaction_date;
        m << ".";
        const std::string message = m.str();

        if (announcer_) announcer_(message);
        repo_.mark_politician_trade_public(t.id);

        for (const auto& user : repo_.politician_watchers(to_lower(trim(name)))) {
            if (notifier_) notifier_->send_direct(user, message);
        }
    }
    repo_.set_meta(kLastRunKey, utc_now_iso());
    std::string summary = "politicians: " + std::to_string(all_new.size()) + " new trade(s)";
    if (skipped_paper) summary += ", " + std::to_string(skipped_paper) + " paper filing(s) skipped";
    if (fetch_errors) summary += ", " + std::to_string(fetch_errors) + " filing(s) failed to fetch";
    log_info(summary);
    return all_new.size();
}

}  // namespace strojanka::politicians
