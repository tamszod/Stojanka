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

PoliticianService::PoliticianService(Repository& repo, std::vector<DisclosureSource*> sources, const Config& config)
    : repo_(repo), sources_(std::move(sources)), config_(config) {}

PoliticianService::~PoliticianService() { stop(); }

void PoliticianService::set_announcer(std::function<void(const std::string&)> announcer) { announcer_ = std::move(announcer); }
void PoliticianService::set_notifier(Notifier* notifier) { notifier_ = notifier; }

void PoliticianService::start() {
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] {
        for (;;) {
            try {
                const std::string last = repo_.meta(kLastRunKey);
                if (last.empty() || unix_now() - iso_to_unix(last) >= kCheckEverySeconds) tick();
            } catch (const std::exception& e) {
                log_error(std::string("politicians failed: ") + e.what());
            } catch (...) {
                log_error("politicians failed with an unknown error");
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

std::string format_filing_announcement(const std::vector<PoliticianTradeRecord>& trades, size_t max_lines) {
    if (trades.empty()) return {};
    const auto& first = trades.front();
    std::ostringstream m;
    m << "\xF0\x9F\x8F\x9B\xEF\xB8\x8F **" << first.full_name() << "**";
    if (!first.office.empty()) m << ", " << first.office << ",";
    m << " disclosed " << trades.size() << " trade" << (trades.size() == 1 ? "" : "s")
      << (first.filed_date.empty() ? "" : " filed " + first.filed_date) << ":";
    for (size_t i = 0; i < trades.size() && i < max_lines; ++i) {
        const auto& t = trades[i];
        const bool has_ticker = !t.ticker.empty() && t.ticker != "--";
        m << "\n- " << (t.transaction_type.empty() ? "Transaction" : t.transaction_type) << " **"
          << (has_ticker ? t.ticker : short_company_name(t.asset_name, 48)) << "**";
        if (has_ticker && !t.asset_name.empty()) m << " (" << short_company_name(t.asset_name, 40) << ")";
        m << ", " << (t.owner.empty() ? "Self" : t.owner) << ", " << format_whole_money(t.amount_min)
          << (t.amount_max ? " - " + format_whole_money(*t.amount_max) : "+");
        if (!t.transaction_date.empty()) m << ", traded " << t.transaction_date;
    }
    if (trades.size() > max_lines) {
        m << "\n...and " << trades.size() - max_lines << " more: `/politicians name:" << first.last_name << "`";
    }
    return m.str();
}

void PoliticianService::announce_filing(const std::vector<PoliticianTradeRecord>& trades) {
    if (trades.empty()) return;
    const std::string message = format_filing_announcement(trades);
    if (announcer_) announcer_(message);
    for (const auto& t : trades) repo_.mark_politician_trade_public(t.id);
    if (notifier_) {
        for (const auto& user : repo_.politician_watchers_for(trades.front().full_name())) notifier_->send_direct(user, message);
    }
}

size_t PoliticianService::check(DisclosureSource& source) {
    const std::string chamber = source.chamber();
    std::string error;
    if (!source.start(&error)) {
        log_warn("politicians (" + chamber + "): " + error);
        return 0;
    }
    const std::string today = unix_to_date(unix_now());
    const std::string since = unix_to_date(unix_now() - static_cast<int64_t>(kLookbackDays) * 86400);
    const auto filings = source.list_filings(since, today, &error);
    if (!error.empty()) {
        log_warn("politicians (" + chamber + "): " + error);
        return 0;
    }
    // A first check would otherwise announce 60 days of filings at once.
    const bool quiet = !repo_.has_politician_trades_in(chamber);
    log_info("politicians (" + chamber + "): " + std::to_string(filings.size()) + " filings in the last " +
             std::to_string(kLookbackDays) + " days" + (quiet ? "; first check, storing them without announcing" : ""));

    size_t added_total = 0;
    int skipped_paper = 0, fetch_errors = 0;
    for (const auto& ref : filings) {
        if (!ref.electronic) {
            ++skipped_paper;
            continue;
        }
        if (repo_.has_politician_filing(ref.report_id)) continue;  // already stored on an earlier run
        std::string fetch_error;
        const auto trades = source.fetch_filing(ref, &fetch_error);
        if (!fetch_error.empty()) {
            ++fetch_errors;
            log_warn("politicians (" + chamber + "): " + fetch_error);
            continue;
        }
        const std::string fetched_at = utc_now_iso();
        std::vector<PoliticianTradeRecord> records;
        for (const auto& t : trades) {
            PoliticianTradeRecord r;
            r.chamber = chamber;
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
        added_total += added.size();
        if (!quiet) announce_filing(added);
    }
    std::string summary = "politicians (" + chamber + "): " + std::to_string(added_total) + " new trade(s)";
    if (skipped_paper) summary += ", " + std::to_string(skipped_paper) + " paper filing(s) skipped";
    if (fetch_errors) summary += ", " + std::to_string(fetch_errors) + " filing(s) failed";
    log_info(summary);
    return added_total;
}

size_t PoliticianService::tick() {
    if (running_.exchange(true)) return 0;
    struct Reset {
        std::atomic<bool>& flag;
        ~Reset() { flag = false; }
    } reset{running_};
    size_t total = 0;
    for (DisclosureSource* source : sources_) {
        try {
            total += check(*source);
        } catch (const std::exception& e) {
            log_error("politicians (" + source->chamber() + ") failed: " + e.what());
        }
    }
    repo_.set_meta(kLastRunKey, utc_now_iso());
    return total;
}

}  // namespace strojanka::politicians
