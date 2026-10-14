#include "core/migrations.h"

#include "common/strings.h"
#include "core/log.h"

namespace strojanka {

namespace {

// Version 1: the initial 0.1.0 schema.
const char* kMigration1 = R"SQL(
CREATE TABLE app_meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE users (
    discord_id        TEXT PRIMARY KEY,
    notify_channel_id TEXT,
    created_at        TEXT NOT NULL
);

CREATE TABLE stocks (
    symbol                  TEXT PRIMARY KEY,
    name                    TEXT,
    currency                TEXT NOT NULL DEFAULT 'USD',
    exchange                TEXT,
    category                TEXT,
    trend                   TEXT,
    last_price              REAL,
    last_price_at           TEXT,
    support_level           REAL,
    resistance_level        REAL,
    peak_score              REAL,
    support_score           REAL,
    profile_json            TEXT,
    history_full_refresh_at TEXT,
    created_at              TEXT NOT NULL,
    updated_at              TEXT
);

CREATE TABLE price_history (
    symbol   TEXT NOT NULL,
    bar_date TEXT NOT NULL,
    open     REAL,
    high     REAL,
    low      REAL,
    close    REAL NOT NULL,
    volume   REAL,
    PRIMARY KEY (symbol, bar_date)
) WITHOUT ROWID;

CREATE TABLE watchlist (
    discord_id TEXT NOT NULL,
    symbol     TEXT NOT NULL,
    added_at   TEXT NOT NULL,
    PRIMARY KEY (discord_id, symbol)
);
CREATE INDEX idx_watchlist_symbol ON watchlist(symbol);

CREATE TABLE transactions (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    discord_id   TEXT NOT NULL,
    symbol       TEXT NOT NULL,
    side         TEXT NOT NULL CHECK (side IN ('BUY', 'SELL')),
    shares       REAL NOT NULL CHECK (shares > 0),
    total_amount REAL NOT NULL CHECK (total_amount >= 0),
    created_at   TEXT NOT NULL
);
CREATE INDEX idx_transactions_user_symbol ON transactions(discord_id, symbol);

CREATE TABLE stock_params (
    symbol                   TEXT PRIMARY KEY,
    params_json              TEXT NOT NULL,
    calibration_score        REAL,
    calibrated_at            TEXT NOT NULL,
    calibration_count        INTEGER NOT NULL DEFAULT 0,
    backtest_peak_signals    INTEGER NOT NULL DEFAULT 0,
    backtest_peak_correct    INTEGER NOT NULL DEFAULT 0,
    backtest_support_signals INTEGER NOT NULL DEFAULT 0,
    backtest_support_correct INTEGER NOT NULL DEFAULT 0,
    baseline_peak            REAL,
    baseline_support         REAL,
    backtest_at              TEXT
);

CREATE TABLE signals (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    symbol       TEXT NOT NULL,
    side         TEXT NOT NULL CHECK (side IN ('PEAK', 'SUPPORT')),
    bar_date     TEXT NOT NULL,
    price        REAL NOT NULL,
    score        REAL NOT NULL,
    threshold    REAL NOT NULL,
    leg_key      TEXT NOT NULL,
    move_pct     REAL NOT NULL,
    horizon_bars INTEGER NOT NULL,
    params_json  TEXT,
    outcome      TEXT NOT NULL DEFAULT 'PENDING' CHECK (outcome IN ('PENDING', 'CORRECT', 'WRONG', 'EXPIRED')),
    created_at   TEXT NOT NULL,
    resolved_at  TEXT,
    UNIQUE (symbol, side, leg_key)
);
CREATE INDEX idx_signals_symbol_outcome ON signals(symbol, outcome);

CREATE TABLE notifications (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    signal_id  INTEGER NOT NULL REFERENCES signals(id),
    discord_id TEXT NOT NULL,
    message    TEXT NOT NULL,
    delivered  INTEGER NOT NULL DEFAULT 0,
    error      TEXT,
    created_at TEXT NOT NULL
);
)SQL";

// Version 2: results of the stock screener behind the `suggest` command.
const char* kMigration2 = R"SQL(
CREATE TABLE screen_results (
    symbol           TEXT PRIMARY KEY,
    name             TEXT,
    currency         TEXT NOT NULL DEFAULT 'USD',
    category         TEXT,
    price            REAL,
    peak_signals     INTEGER NOT NULL DEFAULT 0,
    peak_correct     INTEGER NOT NULL DEFAULT 0,
    baseline_peak    REAL,
    support_signals  INTEGER NOT NULL DEFAULT 0,
    support_correct  INTEGER NOT NULL DEFAULT 0,
    baseline_support REAL,
    peak_edge        REAL,
    support_edge     REAL,
    direction        INTEGER NOT NULL DEFAULT 0,
    peak_score       REAL,
    peak_cutoff      REAL,
    support_score    REAL,
    support_cutoff   REAL,
    screened_at      TEXT NOT NULL
);
)SQL";

// Version 3: news headlines, and the news tone recorded with every alert so its value can be learned.
const char* kMigration3 = R"SQL(
CREATE TABLE news_items (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    symbol       TEXT NOT NULL,
    title        TEXT NOT NULL,
    link         TEXT,
    source       TEXT,
    published_at TEXT NOT NULL,
    sentiment    REAL NOT NULL,
    red_flag     INTEGER NOT NULL DEFAULT 0,
    title_key    TEXT NOT NULL,
    fetched_at   TEXT NOT NULL,
    UNIQUE (symbol, title_key)
);
CREATE INDEX idx_news_symbol_time ON news_items(symbol, published_at);
ALTER TABLE signals ADD COLUMN news_sentiment REAL;
ALTER TABLE stocks ADD COLUMN news_fetched_at TEXT;
)SQL";

// Version 4: bot admins (they receive warnings such as low disk space and can move the database).
const char* kMigration4 = R"SQL(
CREATE TABLE admins (
    discord_id TEXT PRIMARY KEY,
    added_by   TEXT,
    added_at   TEXT NOT NULL
);
)SQL";

// Version 5: the arena of paper-trading bots ($1,000 of play money each).
const char* kMigration5 = R"SQL(
CREATE TABLE arena_bots (
    name           TEXT PRIMARY KEY,
    stage          TEXT NOT NULL,
    cash           REAL NOT NULL,
    peak_value     REAL NOT NULL,
    last_date      TEXT,
    live_started   TEXT,
    stage_changed  TEXT,
    backtest_json  TEXT,
    backtested_at  TEXT
);
CREATE TABLE arena_positions (
    bot        TEXT NOT NULL,
    symbol     TEXT NOT NULL,
    shares     REAL NOT NULL,
    cost       REAL NOT NULL,
    entry_date TEXT NOT NULL,
    PRIMARY KEY (bot, symbol)
);
CREATE TABLE arena_orders (
    bot    TEXT NOT NULL,
    symbol TEXT NOT NULL,
    buy    INTEGER NOT NULL,
    PRIMARY KEY (bot, symbol)
);
CREATE TABLE arena_trades (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    bot        TEXT NOT NULL,
    symbol     TEXT NOT NULL,
    buy        INTEGER NOT NULL,
    shares     REAL NOT NULL,
    price      REAL NOT NULL,
    value      REAL NOT NULL,
    profit     REAL NOT NULL,
    trade_date TEXT NOT NULL,
    public     INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX idx_arena_trades_bot ON arena_trades(bot, id);
CREATE TABLE arena_equity (
    bot      TEXT NOT NULL,
    day      TEXT NOT NULL,
    value    REAL NOT NULL,
    PRIMARY KEY (bot, day)
) WITHOUT ROWID;
)SQL";

// Version 6: trained AI models (Pythia), so a restart does not need to retrain.
const char* kMigration6 = R"SQL(
CREATE TABLE ml_models (
    name        TEXT PRIMARY KEY,
    model_json  TEXT NOT NULL,
    report_json TEXT NOT NULL,
    trained_at  TEXT NOT NULL
);
)SQL";

// Version 7: politician stock trades (Senate PTR disclosures) and who personally watches whom.
const char* kMigration7 = R"SQL(
CREATE TABLE politician_trades (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    chamber          TEXT NOT NULL,
    first_name       TEXT NOT NULL,
    last_name        TEXT NOT NULL,
    office           TEXT,
    report_id        TEXT NOT NULL,
    item_number      INTEGER NOT NULL,
    filed_date       TEXT,
    transaction_date TEXT,
    owner            TEXT,
    ticker           TEXT,
    asset_name       TEXT,
    asset_type       TEXT,
    transaction_type TEXT,
    amount_min       REAL,
    amount_max       REAL,
    has_amount_max   INTEGER NOT NULL DEFAULT 0,
    comment          TEXT,
    fetched_at       TEXT NOT NULL,
    public           INTEGER NOT NULL DEFAULT 0,
    UNIQUE (report_id, item_number)
);
CREATE INDEX idx_politician_trades_name ON politician_trades(last_name, first_name);
CREATE TABLE politician_watch (
    discord_id TEXT NOT NULL,
    politician TEXT NOT NULL,  -- normalized "first last" (lowercase), matched against trades
    added_at   TEXT NOT NULL,
    PRIMARY KEY (discord_id, politician)
);
)SQL";

}  // namespace

const std::vector<Migration>& all_migrations() {
    static const std::vector<Migration> migrations = {
        {1, "initial schema (users, stocks, price history, watchlist, transactions, signals)", kMigration1},
        {2, "stock screener results for the suggest command", kMigration2},
        {3, "news headlines and news tone per alert", kMigration3},
        {4, "bot admins", kMigration4},
        {5, "arena of paper-trading bots", kMigration5},
        {6, "trained AI models", kMigration6},
        {7, "politician stock trades and personal watchlists", kMigration7},
    };
    return migrations;
}

int latest_schema_version() { return all_migrations().back().version; }

MigrationReport migrate_database(Database& db, bool make_backup) {
    std::lock_guard<std::recursive_mutex> lock(db.mutex());
    MigrationReport report;
    report.from_version = db.user_version();
    report.to_version = report.from_version;
    const int latest = latest_schema_version();

    if (report.from_version > latest) {
        throw DatabaseError("database schema version " + std::to_string(report.from_version) +
                            " is newer than this program supports (" + std::to_string(latest) +
                            "). Please install a newer Strojanka.");
    }
    if (report.from_version == latest) return report;

    if (make_backup && report.from_version > 0 && db.path() != ":memory:") {
        std::string stamp = utc_now_iso();
        for (auto& c : stamp) {
            if (c == ':') c = '-';
        }
        report.backup_path = db.path() + ".backup-v" + std::to_string(report.from_version) + "-" + stamp;
        auto st = db.prepare("VACUUM INTO ?1;");
        st.bind(1, report.backup_path);
        st.step();
        log_info("database backed up to " + report.backup_path);
    }

    for (const auto& m : all_migrations()) {
        if (m.version <= report.from_version) continue;
        SqlTransaction tx(db);
        db.exec(m.sql);
        db.set_user_version(m.version);
        auto st = db.prepare("INSERT OR REPLACE INTO app_meta(key, value) VALUES ('schema_migrated_at', ?1);");
        st.bind(1, utc_now_iso());
        st.step();
        tx.commit();
        report.to_version = m.version;
        report.applied.push_back("v" + std::to_string(m.version) + ": " + m.description);
        log_info("applied database migration v" + std::to_string(m.version) + " (" + m.description + ")");
    }
    return report;
}

}  // namespace strojanka
