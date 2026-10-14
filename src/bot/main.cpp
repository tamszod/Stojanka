#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "bot/bot_host.h"
#include "common/config.h"
#include "common/crash_handler.h"
#include "common/platform.h"
#include "common/strings.h"
#include "common/utf8_path.h"
#include "core/admin_service.h"
#include "core/politicians/politician_service.h"
#include "core/arena/arena_service.h"
#include "core/analysis/walk_forward.h"
#include "core/commands.h"
#include "core/database.h"
#include "core/log.h"
#include "core/market/yahoo_provider.h"
#include "core/messages.h"
#include "core/migrations.h"
#include "core/monitor_service.h"
#include "core/news/news_provider.h"
#include "core/repository.h"
#include "core/screener.h"
#include "core/version.h"

namespace fs = std::filesystem;
using namespace strojanka;

namespace {

void print_usage() {
    std::cout << "Strojanka " << STROJANKA_VERSION << " - Discord stock PEAK/SUPPORT watcher\n\n"
              << "Usage: strojanka [options]\n"
              << "  --config <file>    configuration file (default: strojanka.conf next to the program)\n"
              << "  --migrate          create or upgrade the database, then exit\n"
              << "  --analyze <SYMBOL> print an analysis report for a stock without starting Discord\n"
              << "  --news <SYMBOL>    print recent headlines with their tone\n"
              << "  --arena            run the trading-bot arena once and print every bot\n"
              << "  --politicians [name]  check for new US Senate stock trade disclosures and print them\n"
              << "  --move-db <folder> move the database to another folder or drive (bot must be stopped)\n"
              << "  --suggest [buy|sell] screen the stock list now and print the most predictable stocks\n"
              << "  --backtest <SYMBOL> [SYMBOL...]\n"
              << "                     measure out-of-sample accuracy on 10 years of history\n"
              << "  --version          print the version\n"
              << "  --help             show this help\n";
}

int run_backtest(const std::vector<std::string>& symbols) {
    YahooProvider provider;
    analysis::WalkForwardResult total;
    auto pct = [](int ok, int n) { return n ? format_percent(static_cast<double>(ok) / n, 0) : std::string("-"); };
    std::cout << "Walk-forward backtest: re-learn every 20 trading days, call the next 20 blind.\n"
              << "'random' = the score of making that call on every day (the bar to beat).\n\n"
              << "SYMBOL     PEAK right    random    SUPPORT right  random\n";
    auto line = [&pct](const std::string& name, const analysis::WalkForwardResult& r) {
        std::printf("%-10s %3d/%-3d %-5s %-8s  %3d/%-3d %-5s %-6s\n", name.c_str(), r.peak_correct, r.peak_signals,
                    pct(r.peak_correct, r.peak_signals).c_str(), pct(r.baseline_peak_correct, r.baseline_peak_days).c_str(),
                    r.support_correct, r.support_signals, pct(r.support_correct, r.support_signals).c_str(),
                    pct(r.baseline_support_correct, r.baseline_support_days).c_str());
    };
    for (const auto& raw : symbols) {
        const std::string symbol = normalize_symbol(raw);
        std::string error;
        const auto data = provider.fetch(symbol, HistoryRange::TenYears, &error);
        if (!data) {
            std::cout << symbol << ": " << error << "\n";
            continue;
        }
        const auto r = analysis::walk_forward(data->bars);
        total.peak_signals += r.peak_signals;
        total.peak_correct += r.peak_correct;
        total.support_signals += r.support_signals;
        total.support_correct += r.support_correct;
        total.baseline_peak_days += r.baseline_peak_days;
        total.baseline_peak_correct += r.baseline_peak_correct;
        total.baseline_support_days += r.baseline_support_days;
        total.baseline_support_correct += r.baseline_support_correct;
        line(symbol, r);
    }
    line("TOTAL", total);
    return 0;
}

std::string default_config_path() {
    const fs::path beside_exe = utf8_path(executable_dir()) / "strojanka.conf";
    if (fs::exists(beside_exe)) return path_utf8(beside_exe);
    return path_utf8((fs::current_path() / "strojanka.conf"));
}

int run_migrate(const Config& config, const std::string& config_path) {
    const std::string db_path = resolve_config_path(config_path, config.database_path);
    Database db(db_path);
    const auto report = migrate_database(db, true);
    std::cout << "Database: " << db_path << "\n";
    if (report.applied.empty()) {
        std::cout << "Schema is up to date (version " << report.to_version << ").\n";
    } else {
        if (!report.backup_path.empty()) std::cout << "Backup:   " << report.backup_path << "\n";
        std::cout << "Upgraded schema from version " << report.from_version << " to " << report.to_version << ":\n";
        for (const auto& a : report.applied) std::cout << "  - " << a << "\n";
    }
    return 0;
}

int run_analyze(const Config& config, const std::string& config_path, bool have_config, const std::string& symbol) {
    const std::string db_path = have_config ? resolve_config_path(config_path, config.database_path) : ":memory:";
    Database db(db_path);
    migrate_database(db, true);
    Repository repo(db);
    YahooProvider provider;
    news::RssNewsProvider news_provider;
    MonitorService monitor(repo, provider, config, &news_provider);
    std::string error;
    const auto report = monitor.refresh(normalize_symbol(symbol), false, &error);
    if (!report) {
        std::cerr << "Analysis failed: " << error << "\n";
        return 1;
    }
    std::cout << format_report(*report) << "\n";
    return 0;
}

// Runs one arena pass (prices, historical tests, live trading) and prints every bot.
int run_politicians(const Config& config, const std::string& config_path, bool have_config, const std::string& name_filter) {
    const std::string db_path = have_config ? resolve_config_path(config_path, config.database_path) : ":memory:";
    Database db(db_path);
    migrate_database(db, true);
    Repository repo(db);
    politicians::SenateEfdClient source;
    politicians::PoliticianService service(repo, source, config);
    std::cout << "Checking for new US Senate stock trade disclosures (last 60 days)...\n" << std::flush;
    const size_t n = service.tick();
    std::cout << n << " new trade(s) found.\n\n";
    YahooProvider provider;
    MonitorService monitor(repo, provider, config);
    CommandService commands(repo, monitor);
    std::cout << commands.politicians("cli", name_filter).text << "\n";
    return 0;
}

int run_arena(const Config& config, const std::string& config_path, bool have_config) {
    const std::string db_path = have_config ? resolve_config_path(config_path, config.database_path) : ":memory:";
    Database db(db_path);
    migrate_database(db, true);
    Repository repo(db);
    YahooProvider provider;
    arena::ArenaService arena(repo, provider, config);
    ml::Pythia pythia(repo);
    pythia.load();
    arena.set_pythia(&pythia);
    std::cout << "Running the arena (the first run downloads about 10 years of prices for each stock and trains Pythia)...\n"
              << std::flush;
    arena.tick(arena::ArenaService::last_complete_date(unix_now()));
    std::cout << pythia.status_text() << "\n\n" << arena.leaderboard(true) << "\n\n";
    for (const auto& spec : arena::roster()) std::cout << arena.details(spec.name) << "\n";
    return 0;
}

// Moves the database while the bot is stopped (the `admin movedb` command does it while running).
int run_move_db(const Config& config, const std::string& config_path, const std::string& folder) {
    const std::string db_path = resolve_config_path(config_path, config.database_path);
    Database db(db_path);
    migrate_database(db, true);
    Repository repo(db);
    YahooProvider provider;
    MonitorService monitor(repo, provider, config);
    AdminService admin(db, repo, monitor, config, config_path);
    const CommandReply reply = admin.move_database(folder);
    std::cout << reply.text << "\n";
    return reply.ok ? 0 : 1;
}

int run_news(const Config& config, const std::string& config_path, bool have_config, const std::string& symbol) {
    const std::string db_path = have_config ? resolve_config_path(config_path, config.database_path) : ":memory:";
    Database db(db_path);
    migrate_database(db, true);
    Repository repo(db);
    YahooProvider provider;
    news::RssNewsProvider news_provider;
    MonitorService monitor(repo, provider, config, &news_provider);
    CommandService commands(repo, monitor);
    const auto reply = commands.news("cli", symbol);
    std::cout << reply.text << "\n";
    return reply.ok ? 0 : 1;
}

// Screens now (blocking) and prints what `suggest` would answer.
int run_suggest(const Config& config, const std::string& config_path, bool have_config, const std::string& side) {
    const std::string db_path = have_config ? resolve_config_path(config_path, config.database_path) : ":memory:";
    Database db(db_path);
    migrate_database(db, true);
    Repository repo(db);
    YahooProvider provider;
    MonitorService monitor(repo, provider, config);
    Screener screener(repo, provider, config);
    std::cout << "Screening " << screener.universe().size() << " stocks (about a minute)...\n" << std::flush;
    screener.run_now();
    CommandService commands(repo, monitor, &screener);
    std::cout << commands.suggest("cli", side).text << "\n";
    return 0;
}

// Headless server: runs until the process is stopped (Ctrl+C or closing the console).
int run_bot(const std::string& config_path) {
    BotHost host(config_path);
    std::string error;
    if (!host.start(&error)) return 1;
    host.wait();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    install_crash_handler();
    enable_utf8_console();
    std::string config_path;
    std::string analyze_symbol;
    std::string suggest_side;
    std::string news_symbol;
    std::string move_db_folder;
    bool arena_mode = false;
    std::string politicians_filter;
    bool politicians_mode = false;
    bool suggest = false;
    bool migrate = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) config_path = argv[++i];
        else if (arg == "--migrate") migrate = true;
        else if (arg == "--analyze" && i + 1 < argc) analyze_symbol = argv[++i];
        else if (arg == "--news" && i + 1 < argc) news_symbol = argv[++i];
        else if (arg == "--move-db" && i + 1 < argc) move_db_folder = argv[++i];
        else if (arg == "--arena") arena_mode = true;
        else if (arg == "--politicians") {
            politicians_filter = i + 1 < argc && argv[i + 1][0] != '-' ? argv[++i] : "";
            politicians_mode = true;
        }
        else if (arg == "--suggest") {
            suggest_side = i + 1 < argc && argv[i + 1][0] != '-' ? argv[++i] : "";
            suggest = true;
        }
        else if (arg == "--backtest" && i + 1 < argc) {
            std::vector<std::string> symbols(argv + i + 1, argv + argc);
            return run_backtest(symbols);
        }
        else if (arg == "--version") {
            std::cout << STROJANKA_VERSION << "\n";
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n\n";
            print_usage();
            return 2;
        }
    }

    const bool explicit_config = !config_path.empty();
    if (!explicit_config) config_path = default_config_path();
    std::string error;
    Config config;
    const auto loaded = load_config(config_path, &error);
    if (loaded) {
        config = *loaded;
    } else if (explicit_config || (analyze_symbol.empty() && news_symbol.empty() && !suggest && !arena_mode && !politicians_mode)) {
        std::cerr << "Could not load configuration: " << error << "\n";
        return 1;
    }

    log_set_level(parse_log_level(config.log_level));
    if (loaded && analyze_symbol.empty() && news_symbol.empty() && move_db_folder.empty() && !suggest && !arena_mode &&
        !politicians_mode && !migrate &&
        !config.log_file.empty()) {
        log_set_file(resolve_config_path(config_path, config.log_file));
    }

    try {
        if (migrate) return run_migrate(config, config_path);
        if (!move_db_folder.empty()) return run_move_db(config, config_path, move_db_folder);
        if (!analyze_symbol.empty()) return run_analyze(config, config_path, loaded.has_value(), analyze_symbol);
        if (!news_symbol.empty()) return run_news(config, config_path, loaded.has_value(), news_symbol);
        if (arena_mode) return run_arena(config, config_path, loaded.has_value());
        if (politicians_mode) return run_politicians(config, config_path, loaded.has_value(), politicians_filter);
        if (suggest) return run_suggest(config, config_path, loaded.has_value(), suggest_side);
        return run_bot(config_path);
    } catch (const std::exception& e) {
        log_error(e.what());
        return 1;
    }
}
