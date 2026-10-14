// Minimal self-contained test runner (no framework dependency).

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/payload.h"
#include "common/strings.h"
#include "common/utf8_path.h"
#include "core/analysis/analyzer.h"
#include "core/analysis/calibrator.h"
#include "core/analysis/classifier.h"
#include "core/analysis/walk_forward.h"
#include "core/analysis/zigzag.h"
#include "core/messages.h"
#include "core/commands.h"
#include "core/database.h"
#include "core/market/yahoo_provider.h"
#include "core/migrations.h"
#include "core/monitor_service.h"
#include "core/portfolio.h"
#include "core/repository.h"
#include "common/instance_lock.h"
#include "core/admin_service.h"
#include "core/arena/arena_service.h"
#include "core/arena/engine.h"
#include "core/log.h"
#include "core/ml/gbdt.h"
#include "core/ml/pythia.h"
#include "core/politicians/politician_service.h"
#include "core/politicians/senate_efd.h"
#include "core/server_console.h"
#include "core/news/news_provider.h"
#include "core/news/sentiment.h"
#include "core/screener.h"

namespace fs = std::filesystem;
using namespace strojanka;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!(cond)) {                                                                       \
            ++g_failures;                                                                    \
            std::cerr << "  FAILED " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n"; \
        }                                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

fs::path temp_dir() {
    const fs::path p = fs::temp_directory_path() / ("strojanka_tests_" + std::to_string(unix_now()));
    fs::create_directories(p);
    return p;
}

std::vector<Bar> make_bars(const std::vector<double>& closes) {
    std::vector<Bar> bars;
    int64_t t = iso_to_unix("2023-01-02");
    for (double c : closes) {
        Bar b;
        b.date = unix_to_date(t);
        b.open = c;
        b.close = c;
        b.high = c * 1.005;
        b.low = c * 0.995;
        bars.push_back(b);
        t += 86400;
    }
    return bars;
}

// A cyclical stock: 60-day sine cycle of +-15% around 100, plus noise.
std::vector<double> cyclical_closes(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, 0.006);
    std::vector<double> c;
    for (size_t i = 0; i < n; ++i) {
        c.push_back(100.0 * (1 + 0.15 * std::sin(2 * 3.14159265358979 * static_cast<double>(i) / 60.0)) * (1 + noise(rng)));
    }
    return c;
}

class FakeProvider : public MarketDataProvider {
public:
    std::vector<Bar> bars;
    std::optional<ChartData> fetch(const std::string& symbol, HistoryRange, std::string* error) override {
        if (symbol == "NOPE") {
            if (error) *error = "symbol not found";
            return std::nullopt;
        }
        ChartData d;
        d.symbol = symbol;
        d.name = symbol + " Inc.";
        d.bars = bars;
        d.last_price = bars.empty() ? 0 : bars.back().close;
        return d;
    }
};

class RecordingNotifier : public Notifier {
public:
    std::vector<std::pair<std::string, std::string>> sent;
    std::vector<std::pair<std::string, std::string>> direct;
    void send(int64_t, const std::string& user, const std::string& message) override { sent.emplace_back(user, message); }
    void send_direct(const std::string& user, const std::string& message) override { direct.emplace_back(user, message); }
};

void test_strings() {
    CHECK_NEAR(*parse_amount("59.78$"), 59.78, 1e-9);
    CHECK_NEAR(*parse_amount("$1,234.50"), 1234.5, 1e-9);
    CHECK_NEAR(*parse_amount("12,5"), 12.5, 1e-9);
    CHECK_NEAR(*parse_amount("0.858291457"), 0.858291457, 1e-12);
    CHECK(!parse_amount("abc").has_value());
    CHECK(!parse_amount("1.2.3").has_value());
    CHECK(format_money(1234.5) == "$1,234.50");
    CHECK(format_money(-3.1) == "-$3.10");
    CHECK(format_signed_money(8.09) == "+$8.09");
    CHECK(format_shares(0.858291457) == "0.858291457");
    CHECK(format_shares(2.0) == "2");
    CHECK(is_valid_symbol("BRK-B"));
    CHECK(!is_valid_symbol("FC X"));
    CHECK(unix_to_date(iso_to_unix("2024-02-29")) == "2024-02-29");
    CHECK(iso_to_unix("1970-01-02T00:00:00Z") == 86400);
}

void test_config(const fs::path& dir) {
    Config c;
    c.discord_token = "abc.def";
    c.guild_id = "123";
    c.poll_interval_minutes = 7;
    const auto path = path_utf8((dir / "test.conf"));
    std::string err;
    CHECK(save_config(c, path, &err));
    auto loaded = load_config(path, &err);
    CHECK(loaded.has_value());
    CHECK(loaded->discord_token == "abc.def");
    CHECK(loaded->poll_interval_minutes == 7);
    CHECK(resolve_config_path(path, "data/x.db") == path_utf8((dir / "data" / "x.db").lexically_normal()));
}

void test_migrations(const fs::path& dir) {
    const auto path = path_utf8((dir / "migrate.db"));
    {
        Database db(path);
        const auto r = migrate_database(db, false);
        CHECK(r.from_version == 0);
        CHECK(r.to_version == latest_schema_version());
        CHECK(db.user_version() == latest_schema_version());
    }
    {
        Database db(path);
        const auto r = migrate_database(db, true);
        CHECK(r.applied.empty());  // idempotent
    }
    {
        Database db(path);
        db.set_user_version(latest_schema_version() + 1);
        bool threw = false;
        try {
            migrate_database(db, false);
        } catch (const DatabaseError&) {
            threw = true;
        }
        CHECK(threw);  // refuses a database from a newer version
    }
    {
        // Upgrade path from v1: an existing user survives, the newer tables and columns appear.
        const auto old_path = path_utf8(dir / "v1.db");
        Database db(old_path);
        db.exec(all_migrations().front().sql);  // a database exactly as version 1 created it
        db.set_user_version(1);
        db.exec("INSERT INTO users(discord_id, created_at) VALUES ('42', '2026-01-01T00:00:00Z');");
        const auto r = migrate_database(db, false);
        CHECK(r.from_version == 1);
        CHECK(r.to_version == latest_schema_version());
        CHECK(r.applied.size() == static_cast<size_t>(latest_schema_version() - 1));
        auto st = db.prepare("SELECT COUNT(*) FROM users WHERE discord_id = '42';");
        CHECK(st.step() && st.integer(0) == 1);
        db.exec("SELECT COUNT(*) FROM screen_results; SELECT news_sentiment FROM signals; SELECT COUNT(*) FROM news_items;");
    }
}

void test_portfolio() {
    std::vector<TransactionRecord> txs(3);
    txs[0].side = TxSide::Buy;
    txs[0].shares = 2;
    txs[0].total = 100;
    txs[1].side = TxSide::Buy;
    txs[1].shares = 2;
    txs[1].total = 140;  // average cost now 60
    txs[2].side = TxSide::Sell;
    txs[2].shares = 1;
    txs[2].total = 75;
    const Position p = compute_position(txs);
    CHECK_NEAR(p.shares, 3, 1e-9);
    CHECK_NEAR(p.average_cost(), 60, 1e-9);
    CHECK_NEAR(p.realized_profit, 15, 1e-9);
    CHECK_NEAR(p.unrealized_profit(70), 30, 1e-9);
}

void test_zigzag_and_outcome() {
    const auto closes = cyclical_closes(300, 1);
    const auto pivots = analysis::zigzag_pivots(closes, 0.10);
    CHECK(pivots.size() >= 8);  // about 5 cycles -> 2 pivots each
    for (size_t i = 1; i < pivots.size(); ++i) CHECK(pivots[i].is_high != pivots[i - 1].is_high);

    const auto bars = make_bars({100, 100, 99, 97, 94, 93});
    CHECK(analysis::evaluate_outcome(bars, 0, 100, SignalSide::Peak, 0.05, 10) == SignalOutcome::Correct);
    CHECK(analysis::evaluate_outcome(bars, 0, 100, SignalSide::Support, 0.05, 10) == SignalOutcome::Wrong);
    const auto flat = make_bars({100, 100, 100});
    CHECK(analysis::evaluate_outcome(flat, 0, 100, SignalSide::Peak, 0.05, 10) == SignalOutcome::Pending);
    CHECK(analysis::evaluate_outcome(flat, 0, 100, SignalSide::Peak, 0.05, 2) == SignalOutcome::Expired);
}

void test_calibration_learns_cycles() {
    const auto bars = make_bars(cyclical_closes(500, 7));
    const auto result = analysis::calibrate(bars, analysis::AnalysisParams{});
    CHECK(result.enough_data);
    CHECK(result.peak.signals >= 4);
    CHECK(result.support.signals >= 4);
    std::cout << "  cyclical backtest: peak " << result.peak.correct << "/" << result.peak.signals << ", support "
              << result.support.correct << "/" << result.support.signals << "\n";
    CHECK(result.peak.precision > 0.6);
    CHECK(result.support.precision > 0.6);
    const auto profile = analysis::classify(bars);
    CHECK(profile.category == analysis::StockCategory::Cyclical);

    const auto wf = analysis::walk_forward(make_bars(cyclical_closes(900, 11)));
    std::cout << "  cyclical walk-forward: peak " << wf.peak_correct << "/" << wf.peak_signals << ", support "
              << wf.support_correct << "/" << wf.support_signals << "\n";
    CHECK(wf.segments > 20);
    CHECK(has_proven_edge(wf.peak_correct, wf.peak_signals,
                          static_cast<double>(wf.baseline_peak_correct) / wf.baseline_peak_days));
    CHECK(has_proven_edge(wf.support_correct, wf.support_signals,
                          static_cast<double>(wf.baseline_support_correct) / wf.baseline_support_days));
    CHECK(!has_proven_edge(3, 10, 0.30));

    std::vector<SignalRecord> wrong(5);
    for (auto& s : wrong) s.outcome = SignalOutcome::Wrong;
    CHECK(analysis::adjust_bias(0.0, wrong) > 0.0);  // repeated mistakes make the bot stricter
}

void test_uptrend_classification() {
    std::vector<double> c;
    for (int i = 0; i < 400; ++i) c.push_back(50 * std::exp(0.0012 * i) * (1 + 0.004 * std::sin(i)));
    CHECK(analysis::classify(make_bars(c)).category == analysis::StockCategory::Uptrend);
    CHECK(analysis::short_term_trend(c, 0.01) == analysis::Trend::Increasing);
}

void test_yahoo_parser() {
    const std::string body = R"({"chart":{"result":[{"meta":{"currency":"USD","symbol":"FCX","fullExchangeName":"NYSE",
        "regularMarketPrice":73.69,"gmtoffset":-14400,"longName":"Freeport-McMoRan Inc."},
        "timestamp":[1791207000,1791293400,1791379800],
        "indicators":{"quote":[{"open":[72.2,72.8,null],"high":[73.5,73.3,null],"low":[71.4,70.9,null],
        "close":[72.6,72.56,null],"volume":[100,200,null]}],"adjclose":[{"adjclose":[72.6,72.56,null]}]}}],"error":null}})";
    std::string err;
    const auto d = parse_yahoo_chart(body, &err);
    CHECK(d.has_value());
    if (!d) return;
    CHECK(d->bars.size() == 2);  // null bar skipped
    CHECK(d->name == "Freeport-McMoRan Inc.");
    CHECK_NEAR(d->last_price, 73.69, 1e-9);
    const auto missing = parse_yahoo_chart(R"({"chart":{"result":null,"error":{"code":"Not Found","description":"No data found"}}})", &err);
    CHECK(!missing.has_value());
}

void test_commands_and_alerts(const fs::path& dir) {
    Database db(path_utf8((dir / "commands.db")));
    migrate_database(db, false);
    Repository repo(db);
    FakeProvider provider;
    // Build a series that ends at a fresh cyclical top so a PEAK fires on today's data.
    auto closes = cyclical_closes(495, 3);
    auto bars = make_bars(closes);
    const int64_t shift = unix_now() - iso_to_unix(bars.back().date);
    for (auto& b : bars) b.date = unix_to_date(iso_to_unix(b.date) + shift);
    provider.bars = bars;
    Config config;
    MonitorService monitor(repo, provider, config);
    RecordingNotifier notifier;
    monitor.set_notifier(&notifier);
    CommandService cmd(repo, monitor);

    CHECK(!cmd.monitor("u1", "NOPE").ok);
    CHECK(cmd.monitor("u1", "fcx").ok);
    CHECK(repo.is_watching("u1", "FCX"));
    CHECK(cmd.buy("u2", "FCX", "0.858291457", "59.78$").ok);
    CHECK(repo.is_watching("u2", "FCX"));  // buying adds to monitoring
    CHECK(!cmd.remove("u2", "FCX").ok);   // holds shares
    CHECK(!cmd.sell("u2", "FCX", "5", "100").ok);
    CHECK(cmd.sell("u2", "FCX", "0.858291457", "67.87$").ok);
    const auto profit = cmd.profit("u2", "");
    CHECK(profit.text.find("+$8.09") != std::string::npos);
    CHECK(cmd.remove("u2", "FCX").ok);
    CHECK(cmd.status("u1").ok);
    CHECK(cmd.run_text("u3", "help").text.find("Strojanka") != std::string::npos);
    CHECK(cmd.run_text("u3", "/help").ok);
    CHECK(cmd.run_text("u3", "!HELP").ok);
    CHECK(cmd.run_text("u3", "buy FCX 1 50 $").ok);  // amount may contain a space
    CHECK(cmd.run_text("u3", "profit FCX").ok);
    CHECK(!cmd.run_text("u3", "buy FCX").ok);        // usage hint
    CHECK(!cmd.run_text("u3", "dance").ok);
    CHECK(cmd.run_text("u3", "sell FCX 1 60").text.find("+$10.00") != std::string::npos);
    CHECK(cmd.buy("u1", "FCX", "2", "150").ok);

    // Walk the series forward day by day like the live monitor would.
    size_t peak_alerts = 0, support_alerts = 0;
    for (size_t end = 300; end <= bars.size(); ++end) {
        provider.bars.assign(bars.begin(), bars.begin() + static_cast<std::ptrdiff_t>(end));
        // Pretend the newest bar is today so alerts are allowed.
        const int64_t s = unix_now() - iso_to_unix(provider.bars.back().date);
        for (auto& b : provider.bars) b.date = unix_to_date(iso_to_unix(b.date) + s);
        db.exec("DELETE FROM price_history; DELETE FROM notifications; DELETE FROM signals; UPDATE stocks SET history_full_refresh_at = '';");
        std::string err;
        monitor.refresh("FCX", true, &err);
    }
    for (const auto& [user, msg] : notifier.sent) {
        if (msg.find("has reached PEAK") != std::string::npos) {
            ++peak_alerts;
            CHECK(user == "u1");  // only holders get PEAK alerts
        }
        if (msg.find("has reached SUPPORT") != std::string::npos) ++support_alerts;
    }
    std::cout << "  replay produced " << peak_alerts << " PEAK and " << support_alerts << " SUPPORT alerts\n";
    CHECK(peak_alerts + support_alerts > 0);
}

void test_screener(const fs::path& dir) {
    Database db(path_utf8(dir / "screener.db"));
    migrate_database(db, false);
    Repository repo(db);
    FakeProvider provider;
    provider.bars = make_bars(cyclical_closes(900, 5));
    Config config;
    config.screener_symbols = "aaa, BBB, NOPE, bbb";
    MonitorService monitor(repo, provider, config);
    Screener screener(repo, provider, config);
    CommandService cmd(repo, monitor, &screener);

    CHECK(screener.universe().size() == 3);  // normalised and de-duplicated
    CHECK(Screener::conservative_edge(3, 5, 0.3) < 0);    // too few calls to judge
    CHECK(Screener::conservative_edge(20, 25, 0.3) > 0);  // 80% vs 30%: clearly better
    CHECK(Screener::conservative_edge(10, 25, 0.3) < 0);  // 40% vs 30%: could be luck

    CHECK(screener.run_now() == 2);  // NOPE is skipped
    const auto results = repo.screen_results();
    CHECK(results.size() == 2);
    CHECK(!results.empty() && results.front().best_edge() > 0);  // a clean cycle is predictable
    const auto reply = cmd.suggest("u", "");
    CHECK(reply.ok && reply.text.find("AAA") != std::string::npos);
    CHECK(cmd.run_text("u", "suggest buy").ok);
    CHECK(!cmd.suggest("u", "maybe").ok);

    Database empty_db(":memory:");
    migrate_database(empty_db, false);
    Repository empty_repo(empty_db);
    CommandService no_results(empty_repo, monitor, nullptr);
    CHECK(no_results.suggest("u", "").text.find("first time") != std::string::npos);
}

void test_news_text() {
    using namespace news;
    CHECK(score_headline("Freeport beats estimates and raises guidance").sentiment > 0.5);
    CHECK(score_headline("Acme misses estimates, cuts guidance").sentiment < -0.5);
    CHECK(score_headline("Acme fails to beat estimates").sentiment < 0);  // negation
    const auto flag = score_headline("Acme files for Chapter 11 bankruptcy protection");
    CHECK(flag.red_flag && flag.sentiment < -0.5);
    CHECK(std::fabs(score_headline("Is FCX a Buy Now?").sentiment) < 0.2);
    CHECK(score_headline("Why Are Intel Stocks Falling Today?").sentiment < 0);
    CHECK(score_headline("Bernstein cuts Boeing price target").sentiment < -0.5);
    CHECK(score_headline("JPMorgan raises target for Freeport-McMoRan stock").sentiment > 0.5);
    CHECK(std::fabs(score_headline("Freeport-McMoRan to report third quarter results").sentiment) < 0.01);
    CHECK(title_key("Copper Prices Rise!") == "copperpricesrise");

    CHECK(parse_rfc822("Sun, 11 Oct 2026 16:03:00 +0000") == iso_to_unix("2026-10-11T16:03:00Z"));
    CHECK(parse_rfc822("Sun, 11 Oct 2026 12:03:00 -0400") == iso_to_unix("2026-10-11T16:03:00Z"));
    CHECK(parse_rfc822("Mon, 12 Oct 2026 15:05:21 GMT") == iso_to_unix("2026-10-12T15:05:21Z"));
    CHECK(parse_rfc822("garbage") == 0);
    CHECK(search_name("Freeport-McMoRan Inc.") == "Freeport-McMoRan");
    CHECK(search_name("The Coca-Cola Company") == "Coca-Cola");

    const std::string xml = R"(<rss><channel><title>feed</title>
      <item><title><![CDATA[Freeport Q3 copper output beats estimates]]></title><link>https://x/1</link>
        <pubDate>Sun, 11 Oct 2026 16:03:00 +0000</pubDate></item>
      <item><title>Copper &amp; gold: FCX shares slide - Reuters</title><link>https://x/2</link>
        <pubDate>Mon, 12 Oct 2026 15:05:21 GMT</pubDate><source url="https://reuters.com">Reuters</source></item>
    </channel></rss>)";
    const auto items = parse_rss(xml);
    CHECK(items.size() == 2);
    if (items.size() == 2) {
        CHECK(items[0].title == "Freeport Q3 copper output beats estimates");
        CHECK(items[1].title == "Copper & gold: FCX shares slide - Reuters");
        CHECK(items[1].source == "Reuters");
    }

    const int64_t now = iso_to_unix("2026-10-12T12:00:00Z");
    std::vector<NewsItem> news_items(3);
    news_items[0].published_at = "2026-10-12T10:00:00Z";
    news_items[0].sentiment = -0.8;
    news_items[1].published_at = "2026-10-11T12:00:00Z";
    news_items[1].sentiment = -0.4;
    news_items[1].red_flag = true;
    news_items[1].title = "fraud probe";
    news_items[2].published_at = "2026-09-01T12:00:00Z";  // outside the 3-day window
    news_items[2].sentiment = 1.0;
    const NewsSummary s = summarize(news_items, now);
    CHECK(s.headlines == 2);
    CHECK(s.sentiment < -0.2 && s.sentiment > -0.8);  // negative, shrunk because there are only two
    CHECK(s.red_flags.size() == 1);
    CHECK(describe(0.5) == "positive" && describe(-0.1) == "slightly negative" && describe(0.0) == "neutral");

    // Learning: news that agreed with the call was right far more often -> weight goes up.
    std::vector<SignalRecord> graded;
    for (int i = 0; i < 12; ++i) {
        SignalRecord agree;
        agree.side = SignalSide::Support;
        agree.news_sentiment = 0.5;
        agree.outcome = i < 9 ? SignalOutcome::Correct : SignalOutcome::Wrong;
        SignalRecord disagree;
        disagree.side = SignalSide::Peak;
        disagree.news_sentiment = 0.5;
        disagree.outcome = i < 3 ? SignalOutcome::Correct : SignalOutcome::Wrong;
        graded.push_back(agree);
        graded.push_back(disagree);
    }
    CHECK(adjust_news_weight(kDefaultNewsWeight, graded) > kDefaultNewsWeight);
    for (auto& g : graded) g.outcome = g.side == SignalSide::Support ? SignalOutcome::Wrong : SignalOutcome::Correct;
    CHECK(adjust_news_weight(kDefaultNewsWeight, graded) < kDefaultNewsWeight);
    CHECK(adjust_news_weight(kDefaultNewsWeight, std::vector<SignalRecord>(graded.begin(), graded.begin() + 6)) == kDefaultNewsWeight);
}

class FakeNews : public news::NewsProvider {
public:
    std::vector<NewsItem> items;
    int calls = 0;
    std::vector<NewsItem> fetch(const std::string&, const std::string&, std::string*) override {
        ++calls;
        return items;
    }
};

void test_news_in_monitor(const fs::path& dir) {
    Database db(path_utf8(dir / "news.db"));
    migrate_database(db, false);
    Repository repo(db);
    FakeProvider provider;
    auto bars = make_bars(cyclical_closes(495, 3));
    const int64_t shift = unix_now() - iso_to_unix(bars.back().date);
    for (auto& b : bars) b.date = unix_to_date(iso_to_unix(b.date) + shift);
    provider.bars = bars;
    FakeNews feed;
    for (const char* title : {"Acme beats estimates, raises guidance", "Acme shares jump on record profit"}) {
        NewsItem n;
        n.symbol = "ACME";
        n.title = title;
        n.published_at = unix_to_iso(unix_now() - 3600);
        n.title_key = news::title_key(title);
        n.sentiment = news::score_headline(title).sentiment;
        feed.items.push_back(n);
    }
    Config config;
    MonitorService monitor(repo, provider, config, &feed);
    std::string err;
    const auto report = monitor.refresh("ACME", false, &err);
    CHECK(report.has_value());
    if (!report) return;
    CHECK(feed.calls == 1);
    CHECK(report->news.headlines == 2);
    CHECK(report->news.sentiment > 0);
    CHECK(report->support_score >= report->state.support_score);  // good news helps a buy call
    CHECK(report->peak_score <= report->state.peak_score);
    CHECK(repo.recent_news("ACME", "2000-01-01", 10).size() == 2);
    monitor.refresh("ACME", false, &err);
    CHECK(feed.calls == 1);  // headlines are refreshed at most hourly
    CHECK(format_report(*report).find("News: positive") != std::string::npos);
    CommandService cmd(repo, monitor);
    CHECK(cmd.run_text("u", "news ACME").text.find("beats estimates") != std::string::npos);

    // A red-flag headline blocks SUPPORT alerts: replay the series and count buy alerts with and without it.
    auto count_support_alerts = [&](bool red_flag) {
        Database d(":memory:");
        migrate_database(d, false);
        Repository r(d);
        RecordingNotifier notifier;
        FakeNews flagged;
        MonitorService m(r, provider, config, &flagged);
        m.set_notifier(&notifier);
        r.add_watch("u1", "ACME");
        for (size_t end = 300; end <= bars.size(); ++end) {
            provider.bars.assign(bars.begin(), bars.begin() + static_cast<std::ptrdiff_t>(end));
            const int64_t s = unix_now() - iso_to_unix(provider.bars.back().date);
            for (auto& b : provider.bars) b.date = unix_to_date(iso_to_unix(b.date) + s);
            d.exec("DELETE FROM price_history; DELETE FROM notifications; DELETE FROM signals; DELETE FROM news_items;");
            if (red_flag) {
                NewsItem n;
                n.symbol = "ACME";
                n.title = "SEC probe into Acme accounting fraud";
                n.published_at = unix_to_iso(unix_now() - 3600);
                n.title_key = news::title_key(n.title);
                n.sentiment = -1;
                n.red_flag = true;
                r.add_news({n});
            }
            std::string e;
            m.refresh("ACME", true, &e);
        }
        provider.bars = bars;
        int support = 0;
        for (const auto& [user, msg] : notifier.sent) support += msg.find("SUPPORT") != std::string::npos;
        return support;
    };
    const int without = count_support_alerts(false);
    CHECK(without > 0);
    CHECK(count_support_alerts(true) == 0);
}

void test_admin_and_move(const fs::path& dir) {
    CHECK(parse_user_id("<@355014703254339595>") == "355014703254339595");
    CHECK(parse_user_id("<@!355014703254339595>") == "355014703254339595");
    CHECK(parse_user_id("hello").empty());
    CHECK(parse_user_id("123").empty());
    CHECK(format_bytes(1536) == "2 KB" && format_bytes(5ull * 1024 * 1024) == "5.0 MB");

    // A real file database with data, moved to another folder while open.
    const fs::path a = dir / "drive_a";
    const fs::path b = dir / "drive b";  // spaces in paths must work
    const std::string conf = path_utf8(a / "strojanka.conf");
    Config config;
    config.database_path = path_utf8(a / "data" / "strojanka.db");
    config.low_disk_warning_mb = 1 << 30;  // "always low", to exercise the warning
    std::string err;
    CHECK(save_config(config, conf, &err));
    Database db(config.database_path);
    migrate_database(db, false);
    Repository repo(db);
    repo.add_watch("u1", "FCX");
    FakeProvider provider;
    provider.bars = make_bars(cyclical_closes(200, 2));
    MonitorService monitor(repo, provider, config);
    AdminService admin(db, repo, monitor, config, conf);

    CHECK(!admin.handle("someone", {}).ok);  // not an admin
    admin.set_owners({"355014703254339595"});
    CHECK(admin.is_admin("355014703254339595"));
    CHECK(admin.handle("355014703254339595", {}).text.find("admin panel") != std::string::npos);
    CHECK(admin.handle("355014703254339595", {"add", "<@111111111111111111>"}).ok);
    CHECK(admin.is_admin("111111111111111111"));
    CHECK(!admin.handle("111111111111111111", {"remove", "355014703254339595"}).ok);  // owner stays
    CHECK(admin.admins().size() == 2);

    const auto moved = admin.handle("355014703254339595", {"movedb", path_utf8(b)});
    CHECK(moved.ok);
    CHECK(db.path() == path_utf8(b / "strojanka.db"));
    CHECK(fs::exists(b / "strojanka.db"));
    CHECK(!fs::exists(a / "data" / "strojanka.db"));
    CHECK(repo.is_watching("u1", "FCX"));  // data came along, connection still works
    repo.add_watch("u2", "KO");
    CHECK(load_config(conf, &err)->database_path == path_utf8(b / "strojanka.db"));
    CHECK(!admin.move_database(path_utf8(b)).ok);  // already there
    CHECK(!admin.move_database("relative/folder").ok);
    {
        Database other(path_utf8(dir / "drive_c" / "strojanka.db"));
        other.exec("CREATE TABLE x(a);");
    }
    CHECK(!admin.move_database(path_utf8(dir / "drive_c")).ok);  // refuses to overwrite an existing file
    CHECK(repo.is_watching("u2", "KO"));

    // Health checks: low disk warns every admin once per day; price outage after two failed cycles.
    RecordingNotifier notifier;
    admin.check_health(notifier);
    CHECK(notifier.direct.size() == 2);
    CHECK(!notifier.direct.empty() && notifier.direct[0].second.find("Low disk space") != std::string::npos);
    admin.check_health(notifier);
    CHECK(notifier.direct.size() == 2);  // not repeated within 24 hours

    repo.add_watch("u1", "NOPE");
    repo.remove_watch("u1", "FCX");
    repo.remove_watch("u2", "KO");
    monitor.run_cycle();
    monitor.run_cycle();
    CHECK(monitor.last_cycle().failed_cycles_in_a_row == 2);
    admin.check_health(notifier);
    CHECK(notifier.direct.size() == 4);
    CHECK(notifier.direct.back().second.find("Price data unavailable") != std::string::npos);

    // The screener stores headlines for the stocks it checks.
    FakeNews feed;
    NewsItem n;
    n.symbol = "AAA";
    n.title = "Acme beats estimates";
    n.title_key = news::title_key(n.title);
    n.published_at = utc_now_iso();
    feed.items = {n};
    Config sc;
    sc.screener_symbols = "AAA";
    provider.bars = make_bars(cyclical_closes(900, 5));
    Screener screener(repo, provider, sc, &feed);
    screener.run_now();
    CHECK(feed.calls == 1);
    CHECK(repo.recent_news("AAA", "2000-01-01", 10).size() == 1);
}

void test_server_console(const fs::path& dir) {
    {
        InstanceLock first(path_utf8(dir / "server.db.lock"));
        CHECK(first.acquired());
        InstanceLock second(path_utf8(dir / "server.db.lock"));
        CHECK(!second.acquired());  // a second copy of the bot is refused
    }
    InstanceLock again(path_utf8(dir / "server.db.lock"));
    CHECK(again.acquired());  // released when the first one ended

    std::vector<std::string> seen;
    log_set_listener([&seen](LogLevel, const std::string& line) { seen.push_back(line); });
    log_info("listener test line");
    log_set_listener({});
    log_info("not captured");
    CHECK(seen.size() == 1 && seen[0].find("listener test line") != std::string::npos);

    Database db(":memory:");
    migrate_database(db, false);
    Repository repo(db);
    FakeProvider provider;
    auto bars = make_bars(cyclical_closes(300, 4));
    const int64_t shift = unix_now() - iso_to_unix(bars.back().date);
    for (auto& b : bars) b.date = unix_to_date(iso_to_unix(b.date) + shift);
    provider.bars = bars;
    Config config;
    MonitorService monitor(repo, provider, config);
    Screener screener(repo, provider, config);
    CommandService commands(repo, monitor, &screener);
    AdminService admin(db, repo, monitor, config, path_utf8(dir / "none.conf"));
    commands.set_admin(&admin);
    ServerConsole console(repo, monitor, screener, admin, commands);

    CHECK(console.run("help").text.find("Server commands") != std::string::npos);
    CHECK(console.run("status").text.find("admin panel") != std::string::npos);
    CHECK(console.run("stocks").text.find("No stocks") != std::string::npos);
    CHECK(console.run("as 355014703254339595 monitor FCX").ok);
    CHECK(console.run("as 355014703254339595 buy FCX 2 100").ok);
    const auto rows = console.watched_stocks();
    CHECK(rows.size() == 1 && rows[0].symbol == "FCX" && rows[0].watchers == 1 && rows[0].price > 0);
    CHECK(console.run("stocks").text.find("FCX") != std::string::npos);
    CHECK(console.run("users").text.find("355014703254339595") != std::string::npos);
    CHECK(console.run("check").text.find("1/1") != std::string::npos);
    CHECK(console.run("admin add 111111111111111111").ok);  // the console is trusted
    CHECK(admin.is_admin("111111111111111111"));
    CHECK(console.run("loglevel debug").ok);
    CHECK(!console.run("loglevel loud").ok);
    log_set_level(LogLevel::Info);
    CHECK(!console.run("as nobody status").ok);
    CHECK(!console.run("dance").ok);
    CHECK(console.run("version").text.find("Strojanka") != std::string::npos);
}

std::vector<Bar> trending_bars(size_t n, double daily, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, 0.01);
    std::vector<double> closes;
    double c = 50;
    for (size_t i = 0; i < n; ++i) {
        c *= 1 + daily + noise(rng);
        closes.push_back(c);
    }
    auto bars = make_bars(closes);
    for (size_t i = 1; i < bars.size(); ++i) bars[i].open = bars[i - 1].close;  // opens differ from closes
    return bars;
}

void test_arena_engine() {
    using namespace arena;
    std::map<std::string, std::vector<Bar>> data = {
        {"UPA", trending_bars(700, 0.002, 1)}, {"UPB", trending_bars(700, 0.0015, 2)},
        {"FLAT", make_bars(cyclical_closes(700, 3))}, {"DOWN", trending_bars(700, -0.001, 4)}};
    const Universe u = build_universe(data);
    CHECK(u.calendar.size() == 700);
    CHECK(u.symbols.size() == 4);
    CHECK(roster().size() == 13 && roster()[0].name == "Atlas");
    CHECK(find_spec("Sibylla") && find_spec("Sibylla")->kind == Kind::Oracle);
    CHECK(find_spec("hermes") && find_spec("hermes")->name == "Hermes");
    CHECK(!find_spec("Zeus"));

    // Accounting: Atlas buys everything at the next open, paying the slippage.
    State st;
    std::vector<Fill> fills;
    const Spec& atlas = roster()[0];
    step(atlas, u, 300, st, fills);  // decides only
    CHECK(fills.empty() && st.orders.size() == 4);
    step(atlas, u, 301, st, fills);
    CHECK(fills.size() == 4 && st.positions.size() == 4);
    CHECK(st.cash < 1.0);
    double paid = 0;
    for (const auto& f : fills) {
        paid += f.value;
        const auto* s = &u.symbols[0];
        for (const auto& sym : u.symbols) {
            if (sym.symbol == f.symbol) s = &sym;
        }
        CHECK_NEAR(f.price, s->bars[s->at[301]].open * (1 + kSlippage), 1e-9);  // filled at the open
    }
    CHECK_NEAR(paid, kStartingCash, 1e-6);

    // A trend follower holds the rising stocks and makes money; Atlas too.
    const auto trend = backtest(*find_spec("Mars"), u);
    CHECK(trend.metrics.final_value > kStartingCash);
    CHECK(backtest(atlas, u).passed);

    // No peeking: results on data cut at day N equal the full-data results up to day N.
    std::map<std::string, std::vector<Bar>> cut;
    for (const auto& [sym, bars] : data) cut[sym] = std::vector<Bar>(bars.begin(), bars.begin() + 600);
    const Universe uc = build_universe(cut);
    for (const auto& spec : roster()) {
        State a, b;
        std::vector<Fill> fa, fb;
        double va = 0, vb = 0;
        for (int d = kWarmupDays; d < 600; ++d) {
            va = step(spec, u, d, a, fa);
            vb = step(spec, uc, d, b, fb);
        }
        CHECK_NEAR(va, vb, 1e-6);
        CHECK(fa.size() == fb.size());
    }

    // Selling realises profit = received - paid.
    State s2;
    std::vector<Fill> f2;
    s2.positions["UPA"] = {2.0, 100.0, 300};
    s2.cash = 0;
    s2.orders.push_back({"UPA", false});
    step(*find_spec("Ares"), u, 400, s2, f2);
    CHECK(!f2.empty() && !f2[0].buy);
    const auto& upa = *std::find_if(u.symbols.begin(), u.symbols.end(), [](const SymbolData& s) { return s.symbol == "UPA"; });
    CHECK_NEAR(f2[0].profit, 2.0 * upa.bars[upa.at[400]].open * (1 - kSlippage) - 100.0, 1e-6);
}

void test_arena_service(const fs::path& dir) {
    using namespace arena;
    Database db(path_utf8(dir / "arena.db"));
    migrate_database(db, false);
    Repository repo(db);
    FakeProvider provider;
    provider.bars = trending_bars(700, 0.001, 9);
    Config config;
    config.screener_symbols = "AAA,BBB,CCC";
    ArenaService arena(repo, provider, config);
    std::vector<std::string> announced;
    arena.set_announcer([&announced](const std::string& m) { announced.push_back(m); });

    const std::string last = provider.bars.back().date;
    arena.tick(last);
    const auto bots = repo.arena_bots();
    CHECK(bots.size() == roster().size());
    for (const auto& b : bots) {
        CHECK(!b.backtest_json.empty());
        if (b.name == "Atlas") CHECK(b.stage == "benchmark" && b.last_date == last);
    }
    CHECK(announced.empty());  // nothing public yet
    CHECK(arena.leaderboard(true).find("Atlas") != std::string::npos);
    CHECK(ArenaService::last_complete_date(iso_to_unix("2026-10-13T22:00:00Z")) == "2026-10-13");
    CHECK(ArenaService::last_complete_date(iso_to_unix("2026-10-13T15:00:00Z")) == "2026-10-12");

    // Promotion: a proving bot that beat Atlas over 40 live days without a big fall goes public.
    auto craft = [&](const std::string& name, const std::string& stage, double start, double end) {
        ArenaBotRecord b;
        b.name = name;
        b.stage = stage;
        b.live_started = "2026-01-01";
        b.stage_changed = "2026-01-01";
        b.last_date = last;  // nothing left to trade, so only judging happens
        b.backtested_at = utc_now_iso();
        b.backtest_json = R"({"passed":true,"verdict":"passed"})";
        std::vector<std::pair<std::string, double>> eq;
        for (int i = 0; i <= 45; ++i) eq.emplace_back(unix_to_date(iso_to_unix("2026-01-01") + i * 86400), start + (end - start) * i / 45.0);
        repo.save_arena_state(b, {}, {}, {}, eq);
    };
    craft("Atlas", "benchmark", 1000, 1010);  // the market: +1%
    craft("Hermes", "proving", 1000, 1060);   // +6%
    craft("Ares", "proving", 1000, 980);      // -2%: 3 points behind Atlas, more than the 2 allowed
    arena.tick(last, false);
    for (const auto& b : repo.arena_bots()) {
        if (b.name == "Hermes") CHECK(b.stage == "public");
        if (b.name == "Ares") CHECK(b.stage == "proving");
    }
    CHECK(!announced.empty() && announced.back().find("Hermes") != std::string::npos);
    CHECK(announced.back().find("public") != std::string::npos);

    // Users see public bots and Atlas only.
    MonitorService monitor(repo, provider, config);
    CommandService commands(repo, monitor);
    commands.set_arena(&arena);
    const auto board = commands.run_text("u", "arena").text;
    CHECK(board.find("Hermes") != std::string::npos && board.find("Ares ") == std::string::npos);
    CHECK(commands.run_text("u", "arena ares").text.find("private") != std::string::npos);
    CHECK(commands.run_text("u", "arena hermes").text.find("Greek messenger") != std::string::npos);

    // Demotion: a public bot that falls 30% from its high goes back to proving.
    craft("Hermes", "public", 1000, 700);
    announced.clear();
    arena.tick(last, false);
    for (const auto& b : repo.arena_bots()) {
        if (b.name == "Hermes") CHECK(b.stage == "proving");
    }
    CHECK(!announced.empty() && announced.back().find("back to proving") != std::string::npos);
}

void test_gbdt() {
    std::mt19937 rng(5);
    std::normal_distribution<float> nd(0, 1);
    auto make = [&](int n, std::vector<std::vector<float>>& X, std::vector<uint8_t>& y) {
        for (int i = 0; i < n; ++i) {
            std::vector<float> x(5);
            for (auto& v : x) v = nd(rng);
            if (i % 50 == 0) x[3] = std::numeric_limits<float>::quiet_NaN();  // missing values are allowed
            X.push_back(x);
            y.push_back(x[0] + 0.5f * x[1] + 0.5f * nd(rng) > 0 ? 1 : 0);
        }
    };
    std::vector<std::vector<float>> X, Xt;
    std::vector<uint8_t> y, yt;
    make(6000, X, y);
    make(2000, Xt, yt);
    ml::Gbdt model;
    model.train(X, y);
    std::vector<double> scores;
    for (const auto& x : Xt) scores.push_back(model.predict(x));
    const double a = ml::auc(scores, yt);
    std::cout << "  gbdt holdout AUC " << a << "\n";
    CHECK(a > 0.85);  // it learns a real pattern
    ml::Gbdt copy;
    CHECK(copy.from_json(model.to_json()));
    CHECK_NEAR(copy.predict(Xt[7]), model.predict(Xt[7]), 1e-9);
    CHECK_NEAR(ml::auc({0.1, 0.2, 0.8, 0.9}, {0, 0, 1, 1}), 1.0, 1e-12);
    CHECK_NEAR(ml::auc({0.9, 0.8, 0.2, 0.1}, {0, 0, 1, 1}), 0.0, 1e-12);
}

void test_pythia(const fs::path& dir) {
    std::map<std::string, std::vector<Bar>> data;
    for (int k = 0; k < 12; ++k) data["S" + std::to_string(k)] = make_bars(cyclical_closes(1800, 20 + k));
    arena::Universe u = arena::build_universe(data);

    // No peeking: features at a day are the same whether or not later prices exist.
    std::map<std::string, std::vector<Bar>> cut;
    for (const auto& [sym, bars] : data) cut[sym] = std::vector<Bar>(bars.begin(), bars.begin() + 1500);
    arena::Universe uc = arena::build_universe(cut);

    Database db(path_utf8(dir / "pythia.db"));
    migrate_database(db, false);
    Repository repo(db);
    ml::Pythia pythia(repo);
    CHECK(!pythia.ready());
    pythia.train(u);
    CHECK(pythia.ready());
    const auto r = pythia.report();
    std::cout << "  pythia on cycles: AUC up " << r.auc_up << ", top 10% " << r.top_up << " vs base " << r.base_up << "\n";
    CHECK(r.folds >= 1 && r.oos_samples > 1000);
    CHECK(r.auc_up > 0.6);  // regular cycles are predictable, so it must beat chance here
    // Years used only for training get no predictions, so the oracle bots cannot trade on seen answers.
    const auto& s0 = u.symbols[0];
    CHECK(std::isnan(s0.oracle_up[400]));
    CHECK(!std::isnan(s0.oracle_up.back()));
    CHECK(s0.bars[400].date < r.tested_from);

    const auto p = pythia.predict("S0", data["S0"]);
    CHECK(p.valid && p.up >= 0 && p.up <= 1 && p.move > 0);
    ml::Pythia reloaded(repo);
    CHECK(reloaded.load());
    CHECK_NEAR(reloaded.predict("S0", data["S0"]).up, p.up, 1e-9);
    CHECK(pythia.status_text().find("years it never saw") != std::string::npos);

    // No peeking: every feature of day 1499 is identical with and without the later 300 days.
    const auto ctx = ml::Pythia::market_context(uc);
    const auto with_future = ml::Pythia::features(u.symbols[3], ctx);
    const auto without = ml::Pythia::features(uc.symbols[3], ctx);
    bool same = true;
    for (size_t j = 0; j < with_future[1499].size(); ++j) {
        const float a = with_future[1499][j], b = without[1499][j];
        if (!(a == b || (std::isnan(a) && std::isnan(b)))) same = false;
    }
    CHECK(same);
    CHECK(ml::Pythia::feature_names().size() == with_future[1499].size());
}

void test_senate_efd_parser() {
    using namespace politicians;
    // A realistic electronic PTR transaction table: header matches by name, includes HTML
    // entities, a closed amount range and an open-ended one, and sits among unrelated markup
    // (a nav table before it) to check the table-selection logic doesn't grab the wrong one.
    const std::string html = R"HTML(
<html><body>
<table class="nav"><tr><td>Home</td><td>Search</td></tr></table>
<div class="filing">
<h1>Jane Q. Senator</h1>
<table class="table">
<thead><tr><th>#</th><th>Transaction Date</th><th>Owner</th><th>Ticker</th><th>Asset Name</th>
<th>Asset Type</th><th>Type</th><th>Amount</th><th>Comment</th></tr></thead>
<tbody>
<tr><td>1</td><td>03/14/2026</td><td>Self</td><td>AAPL</td><td>Apple Inc. &amp; Co.</td>
<td>Stock</td><td>Purchase</td><td>$1,001 - $15,000</td><td></td></tr>
<tr><td>2</td><td>03/15/2026</td><td>Spouse</td><td>--</td><td>Big Fund XYZ</td>
<td>Municipal Security</td><td>Sale (Partial)</td><td>Over $50,000,000</td><td>see note</td></tr>
</tbody>
</table>
</div>
</body></html>
)HTML";
    const auto trades = parse_transaction_table(html, "abc123");
    CHECK(trades.size() == 2);
    if (trades.size() != 2) return;
    CHECK(trades[0].report_id == "abc123");
    CHECK(trades[0].item_number == 1);
    CHECK(trades[0].transaction_date == "2026-03-14");
    CHECK(trades[0].owner == "Self");
    CHECK(trades[0].ticker == "AAPL");
    CHECK(trades[0].asset_name == "Apple Inc. & Co.");  // entity decoded
    CHECK(trades[0].transaction_type == "Purchase");
    CHECK_NEAR(trades[0].amount_min, 1001, 1e-9);
    CHECK(trades[0].amount_max.has_value() && std::fabs(*trades[0].amount_max - 15000) < 1e-9);

    CHECK(trades[1].item_number == 2);
    CHECK(trades[1].owner == "Spouse");
    CHECK(trades[1].transaction_type == "Sale (Partial)");
    CHECK(trades[1].comment == "see note");
    CHECK_NEAR(trades[1].amount_min, 50000000, 1e-6);
    CHECK(!trades[1].amount_max.has_value());  // open-ended range

    // A table with none of the recognizable headers and fewer than 8 columns in its first
    // body row should be rejected entirely rather than misread as transactions.
    const std::string junk = "<html><table><tr><td>a</td><td>b</td></tr><tr><td>c</td><td>d</td></tr></table></html>";
    CHECK(parse_transaction_table(junk, "x").empty());

    // The 9-column built-in header list kicks in when the real header row is too short.
    const std::string short_header =
        "<table><tr><th>Col</th></tr>"
        "<tr><td>1</td><td>01/02/2026</td><td>Self</td><td>MSFT</td><td>Microsoft</td><td>Stock</td><td>Purchase</td>"
        "<td>$1,001 - $15,000</td><td></td></tr></table>";
    const auto fallback = parse_transaction_table(short_header, "y");
    CHECK(fallback.size() == 1);
    if (!fallback.empty()) {
        CHECK(fallback[0].ticker == "MSFT");
        CHECK(fallback[0].transaction_date == "2026-01-02");
    }
}

PoliticianTradeRecord make_senate_trade(const std::string& first, const std::string& last, const std::string& report_id,
                                        int item, const std::string& ticker, double amount_min) {
    PoliticianTradeRecord r;
    r.chamber = "Senate";
    r.first_name = first;
    r.last_name = last;
    r.office = "Senator";
    r.report_id = report_id;
    r.item_number = item;
    r.filed_date = "2026-10-01";
    r.transaction_date = "2026-09-15";
    r.owner = "Self";
    r.ticker = ticker;
    r.asset_name = ticker + " Inc.";
    r.asset_type = "Stock";
    r.transaction_type = "Purchase";
    r.amount_min = amount_min;
    r.amount_max = amount_min * 10;
    r.fetched_at = utc_now_iso();
    return r;
}

void test_politician_repository(const fs::path& dir) {
    Database db(path_utf8(dir / "politicians.db"));
    migrate_database(db, false);
    Repository repo(db);

    const auto added = repo.add_politician_trades(
        {make_senate_trade("Jane", "Doe", "rep1", 1, "AAPL", 1001), make_senate_trade("Jane", "Doe", "rep1", 2, "MSFT", 15001)});
    CHECK(added.size() == 2);
    CHECK(added[0].id > 0 && added[1].id > added[0].id);

    // Re-adding the same (report_id, item_number) is silently ignored, not duplicated.
    const auto readded = repo.add_politician_trades({make_senate_trade("Jane", "Doe", "rep1", 1, "AAPL", 1001)});
    CHECK(readded.empty());
    CHECK(repo.recent_politician_trades("", 100).size() == 2);

    CHECK(repo.has_politician_filing("rep1"));
    CHECK(!repo.has_politician_filing("rep-unseen"));

    repo.add_politician_trades({make_senate_trade("John", "Smith", "rep2", 1, "KO", 50000)});
    CHECK(repo.recent_politician_trades("", 100).size() == 3);
    const auto filtered = repo.recent_politician_trades("jane", 100);
    CHECK(filtered.size() == 2);
    CHECK(repo.recent_politician_trades("nobody", 100).empty());
    const auto names = repo.known_politician_names();
    CHECK(names.size() == 2);

    repo.mark_politician_trade_public(added[0].id);
    const auto after_mark = repo.recent_politician_trades("jane", 100);
    CHECK(std::any_of(after_mark.begin(), after_mark.end(), [&](const PoliticianTradeRecord& t) { return t.id == added[0].id && t.is_public; }));

    CHECK(repo.add_politician_watch("u1", "jane doe"));
    CHECK(!repo.add_politician_watch("u1", "jane doe"));  // already watching
    CHECK(repo.add_politician_watch("u2", "jane doe"));
    CHECK(repo.add_politician_watch("u1", "john smith"));
    CHECK(repo.user_politician_watchlist("u1").size() == 2);
    const auto watchers = repo.politician_watchers("jane doe");
    CHECK(watchers.size() == 2);
    CHECK(repo.remove_politician_watch("u1", "jane doe"));
    CHECK(!repo.remove_politician_watch("u1", "jane doe"));
    CHECK(repo.politician_watchers("jane doe").size() == 1);
}

class FakeSenateSource : public politicians::SenateSource {
public:
    bool start_ok = true;
    std::string start_error;
    std::vector<politicians::FilingRef> filings;
    std::map<std::string, std::vector<politicians::SenateTrade>> trades_by_report;
    int start_calls = 0, list_calls = 0, fetch_calls = 0;

    bool start(std::string* error) override {
        ++start_calls;
        if (!start_ok && error) *error = start_error;
        return start_ok;
    }
    std::vector<politicians::FilingRef> list_filings(const std::string&, const std::string&, std::string*) override {
        ++list_calls;
        return filings;
    }
    std::vector<politicians::SenateTrade> fetch_filing(const politicians::FilingRef& ref, std::string*) override {
        ++fetch_calls;
        const auto it = trades_by_report.find(ref.report_id);
        return it == trades_by_report.end() ? std::vector<politicians::SenateTrade>{} : it->second;
    }
};

void test_politician_service(const fs::path& dir) {
    using namespace politicians;
    Database db(path_utf8(dir / "politician_service.db"));
    migrate_database(db, false);
    Repository repo(db);
    Config config;
    FakeSenateSource source;

    FilingRef electronic;
    electronic.first_name = "Jane";
    electronic.last_name = "Doe";
    electronic.office = "Senator (DE)";
    electronic.filed_date = "2026-10-01";
    electronic.report_id = "elec-1";
    electronic.url = "https://efdsearch.senate.gov/search/view/ptr/elec-1/";
    electronic.electronic = true;
    FilingRef paper;
    paper.first_name = "Old";
    paper.last_name = "Timer";
    paper.report_id = "paper-1";
    paper.electronic = false;
    source.filings = {electronic, paper};

    SenateTrade t;
    t.report_id = "elec-1";
    t.item_number = 1;
    t.transaction_date = "2026-09-20";
    t.owner = "Self";
    t.ticker = "AAPL";
    t.asset_name = "Apple Inc.";
    t.transaction_type = "Purchase";
    t.amount_min = 1001;
    t.amount_max = 15000;
    source.trades_by_report["elec-1"] = {t};

    PoliticianService service(repo, source, config);
    std::vector<std::string> announced;
    std::vector<std::pair<std::string, std::string>> dmed;
    service.set_announcer([&](const std::string& m) { announced.push_back(m); });
    class TestNotifier : public Notifier {
    public:
        std::vector<std::pair<std::string, std::string>>* dmed;
        void send(int64_t, const std::string&, const std::string&) override {}
        void send_direct(const std::string& user, const std::string& message) override { dmed->emplace_back(user, message); }
    } notifier;
    notifier.dmed = &dmed;
    service.set_notifier(&notifier);

    repo.add_politician_watch("watcher1", "jane doe");

    CHECK(service.tick() == 1);
    CHECK(source.start_calls == 1 && source.list_calls == 1 && source.fetch_calls == 1);  // paper filing never fetched
    CHECK(announced.size() == 1);
    CHECK(announced[0].find("Jane Doe") != std::string::npos);
    CHECK(announced[0].find("AAPL") != std::string::npos);
    CHECK(dmed.size() == 1 && dmed[0].first == "watcher1");
    CHECK(repo.recent_politician_trades("", 10).size() == 1);
    CHECK(repo.recent_politician_trades("", 10).front().is_public);

    // Running again does not re-fetch the already-ingested filing, and announces nothing new.
    CHECK(service.tick() == 0);
    CHECK(source.fetch_calls == 1);
    CHECK(announced.size() == 1);

    // A failed handshake (e.g. the Akamai block) is handled gracefully, not thrown.
    FakeSenateSource failing;
    failing.start_ok = false;
    failing.start_error = "HTTP 403";
    PoliticianService failing_service(repo, failing, config);
    CHECK(failing_service.tick() == 0);
}

void test_politician_commands(const fs::path& dir) {
    Database db(path_utf8(dir / "politician_commands.db"));
    migrate_database(db, false);
    Repository repo(db);
    FakeProvider provider;
    Config config;
    MonitorService monitor(repo, provider, config);
    CommandService cmd(repo, monitor);

    CHECK(cmd.run_text("u1", "politicians").text.find("No politician trades") != std::string::npos);
    repo.add_politician_trades({make_senate_trade("Jane", "Doe", "rep1", 1, "AAPL", 1001)});
    const auto listing = cmd.run_text("u1", "politicians");
    CHECK(listing.ok && listing.text.find("Jane Doe") != std::string::npos && listing.text.find("AAPL") != std::string::npos);
    CHECK(cmd.run_text("u1", "politicians Smith").text.find("No disclosed trades") != std::string::npos);

    CHECK(!cmd.run_text("u1", "monitor politician").ok);  // missing name
    const auto watch = cmd.run_text("u1", "monitor politician Jane Doe");
    CHECK(watch.ok && watch.text.find("Jane Doe") != std::string::npos);
    CHECK(cmd.run_text("u1", "monitor politician Jane Doe").text.find("already watching") != std::string::npos);
    CHECK(repo.user_politician_watchlist("u1") == std::vector<std::string>{"jane doe"});
    CHECK(cmd.run_text("u1", "remove politician Jane Doe").ok);
    CHECK(!cmd.run_text("u1", "remove politician Jane Doe").ok);  // not watching anymore

    // A plain stock symbol must still go through the normal /monitor path, unaffected.
    provider.bars = make_bars(cyclical_closes(200, 2));
    CHECK(cmd.run_text("u1", "monitor FCX").ok);
    CHECK(repo.is_watching("u1", "FCX"));
}

void test_payload(const fs::path& dir) {
    const fs::path stage = dir / "stage";
    fs::create_directories(stage / "docs");
    std::ofstream(stage / "a.txt") << "hello";
    std::ofstream(stage / "docs" / "b.md") << "# doc";
    std::ofstream(dir / "stub.bin") << "STUBSTUBSTUB";
    std::string err;
    const auto out = path_utf8((dir / "installer.bin"));
    CHECK(payload::pack(path_utf8((dir / "stub.bin")), path_utf8(stage), out, &err));
    std::vector<payload::Entry> entries;
    CHECK(payload::read_index(out, entries, &err));
    CHECK(entries.size() == 2);
    for (const auto& e : entries) CHECK(payload::extract(out, e, path_utf8((dir / "out" / e.path)), &err));
    std::ifstream in(dir / "out" / "docs" / "b.md");
    std::string text;
    std::getline(in, text);
    CHECK(text == "# doc");
}

}  // namespace

int main() {
    const fs::path dir = temp_dir();
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"strings", test_strings},
        {"config", [&] { test_config(dir); }},
        {"migrations", [&] { test_migrations(dir); }},
        {"portfolio", test_portfolio},
        {"zigzag and outcomes", test_zigzag_and_outcome},
        {"calibration learns cycles", test_calibration_learns_cycles},
        {"uptrend classification", test_uptrend_classification},
        {"yahoo parser", test_yahoo_parser},
        {"commands and alerts", [&] { test_commands_and_alerts(dir); }},
        {"screener and suggest", [&] { test_screener(dir); }},
        {"news text, feeds and learning", test_news_text},
        {"news in the monitor", [&] { test_news_in_monitor(dir); }},
        {"admin panel, warnings and database move", [&] { test_admin_and_move(dir); }},
        {"server console, instance lock, log listener", [&] { test_server_console(dir); }},
        {"arena engine: accounting, no look-ahead", test_arena_engine},
        {"arena service: stages, promotion, demotion", [&] { test_arena_service(dir); }},
        {"gbdt learner", test_gbdt},
        {"pythia: walk-forward, no peeking, save/load", [&] { test_pythia(dir); }},
        {"senate EFD transaction table parser", test_senate_efd_parser},
        {"politician trade repository", [&] { test_politician_repository(dir); }},
        {"politician service: fetch, dedup, announce, DM", [&] { test_politician_service(dir); }},
        {"politician commands: monitor/remove/list", [&] { test_politician_commands(dir); }},
        {"payload", [&] { test_payload(dir); }},
    };
    for (const auto& [name, fn] : tests) {
        std::cout << "[test] " << name << "\n";
        try {
            fn();
        } catch (const std::exception& e) {
            ++g_failures;
            std::cerr << "  EXCEPTION " << e.what() << "\n";
        }
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::cout << (g_failures ? "FAILED: " : "OK: ") << g_checks - g_failures << "/" << g_checks << " checks passed\n";
    return g_failures ? 1 : 0;
}
