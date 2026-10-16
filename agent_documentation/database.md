# Database

SQLite, one file (default `<install>/data/strojanka.db`, path in `strojanka.conf: database_path`).
Opened with WAL journal (so `-wal`/`-shm` side files appear while running), foreign keys on.
Schema version = `PRAGMA user_version`. Current: **7** (`src/core/migrations.cpp`; v2 = `screen_results`; v3 = `news_items`, `signals.news_sentiment`, `stocks.news_fetched_at`; v4 = `admins`; v5 = arena tables; v6 = `ml_models`; v7 = `politician_trades`, `politician_watch`).
Location can change at runtime: `Database::move_to(new_path)` (VACUUM INTO → integrity_check → swap handle under the mutex → delete old db/-wal/-shm); `AdminService::move_database` also rewrites `database_path` in strojanka.conf.

## Migration rules
- `migrate_database(db, make_backup)` runs at every bot start and from `strojanka --migrate` (installer).
- If `user_version > latest`: throws (DB from newer app). If `< latest` and `> 0`: `VACUUM INTO "<db>.backup-v<old>-<ts>"` first.
- Each migration runs in its own `SqlTransaction`, then sets `user_version` and `app_meta.schema_migrated_at`.
- To change schema: append `{N+1, "description", R"SQL(...)SQL"}`. Use `ALTER TABLE ... ADD COLUMN` with defaults, or create-copy-drop-rename for bigger changes. Never edit a released migration. Add a test in `tests/test_main.cpp` (`test_migrations`).

## Tables (v1)
| Table | Purpose | Written by | Notes |
|---|---|---|---|
| `app_meta(key PK, value)` | misc metadata | migrations | `schema_migrated_at`; `arena_channel_id` (`AdminService::kPublicChannelKey` — shared public channel for arena *and* politician announcements, legacy key name kept for compatibility); `politicians_last_run`; `run_state` / `run_started_at` (`core/run_state.h`: how the last run ended); `last_backup_at`, `last_backup_path`, `backup_watch_since`, `backup_reminded_at` (backups and their reminders, `admin_service.cpp`) |
| `users(discord_id PK, notify_channel_id, created_at)` | one row per Discord user | `Repository::ensure_user` on every command | channel = fallback when DMs fail |
| `stocks(symbol PK, name, currency, exchange, category, trend, last_price, last_price_at, support_level, resistance_level, peak_score, support_score, profile_json, history_full_refresh_at, created_at, updated_at)` | latest state per symbol (shared by all users) | `MonitorService::refresh` | `trend` ∈ increasing/decreasing/stagnating; `category` = `category_name()` text |
| `price_history(symbol, bar_date, open, high, low, close, volume) PK(symbol,bar_date) WITHOUT ROWID` | daily adjusted bars, up to 10y | `upsert_bars` | adjusted by Yahoo adjclose; full refetch daily keeps adjustments consistent |
| `watchlist(discord_id, symbol, added_at) PK(discord_id,symbol)` | who watches what | `/monitor`, `/buy` (auto-add), `/remove` | index on symbol |
| `transactions(id, discord_id, symbol, side BUY/SELL, shares>0, total_amount>=0, created_at)` | user trades; `total_amount` = TOTAL money paid/received | `/buy`, `/sell` | positions are recomputed from here (`compute_position`), never stored |
| `stock_params(symbol PK, params_json, calibration_score, calibrated_at, calibration_count, backtest_peak_signals, backtest_peak_correct, backtest_support_signals, backtest_support_correct, baseline_peak, baseline_support, backtest_at)` | learned `AnalysisParams` (json) + walk-forward record | `refresh` step 4 | `params_json` keys: swing_pct, rsi_high, rsi_low, peak_threshold, support_threshold, peak_bias, support_bias, move_pct, horizon_bars |
| `signals(id, symbol, side PEAK/SUPPORT, bar_date, price, score, threshold, leg_key, move_pct, horizon_bars, params_json, outcome PENDING/CORRECT/WRONG/EXPIRED, created_at, resolved_at) UNIQUE(symbol,side,leg_key)` | every alert fired, later graded | `emit`, grading in `refresh` step 3 | `leg_key` = date of the pivot that started the swing |
| `screen_results(symbol PK, name, currency, category, price, peak_signals, peak_correct, baseline_peak, support_signals, support_correct, baseline_support, peak_edge, support_edge, direction, peak_score, peak_cutoff, support_score, support_cutoff, screened_at)` (v2) | latest screener run, one row per screened stock | `Screener::run_now` → `replace_screen_results` (delete all + insert, one transaction) | `*_edge` = Wilson 90% lower bound − baseline, −1 if < 8 calls; last run time in `app_meta.screener_last_run` |
| `news_items(id, symbol, title, link, source, published_at, sentiment, red_flag, title_key, fetched_at) UNIQUE(symbol,title_key)` (v3) | every headline ever fetched (history for future news back-tests) | `MonitorService::refresh` step 4 via `add_news` (INSERT OR IGNORE) | `title_key` = first 60 lowercase alnum chars; index (symbol, published_at). Also v3: `signals.news_sentiment` (NULL = no headlines), `stocks.news_fetched_at`; learned weight in `app_meta.news_weight` (+ `news_weight_samples`) |
| `admins(discord_id PK, added_by, added_at)` (v4) | admins added with `admin add` | `AdminService` | application owner/team members are admins in memory only (`set_owners`, from `current_application_get` on ready); warning throttles in `app_meta.disk_warned_at` / `outage_warned_at` |
| `arena_bots(name PK, stage, cash, peak_value, last_date, live_started, stage_changed, backtest_json, backtested_at)` (v5) | one row per roster bot; stage ∈ benchmark/training/proving/public | `ArenaService` (`save_arena_bot`, `save_arena_state`) | dates are trading days (bar dates) except `backtested_at` (UTC ISO) |
| `arena_positions(bot, symbol, shares, cost, entry_date)`, `arena_orders(bot, symbol, buy)` (v5) | live portfolio and orders waiting for the next open | `save_arena_state` (replaces both per bot in one transaction) | `reset_arena_portfolio` clears them when a bot starts proving |
| `arena_trades(id, bot, symbol, buy, shares, price, value, profit, trade_date, public)`, `arena_equity(bot, day, value)` (v5) | live fills and daily closing values | `save_arena_state` | equity drives live return, drawdown, vs-Atlas and promotion |
| `ml_models(name PK, model_json, report_json, trained_at)` (v6) | trained AI models; `pythia` row = {up, down trees, recent market context} + walk-forward report | `Pythia::train` → `save_model`; `Pythia::load` at BotHost start | a few MB of JSON |
| `politician_trades(id, chamber, first_name, last_name, office, filed_date, report_id, item_number, transaction_date, owner, ticker, asset_name, asset_type, transaction_type, amount_min, amount_max, has_amount_max, comment, fetched_at, is_public) UNIQUE(report_id,item_number)` (v7) | every parsed Senate PTR transaction, deduplicated | `PoliticianService::tick` → `add_politician_trades` (INSERT OR IGNORE, only newly-inserted rows returned with `id` set) | `has_amount_max` flags an open-ended range ("Over $50,000,000") since SQLite has no nullable-REAL marker; index on `(last_name, first_name)` |
| `politician_watch(discord_id, politician) PK(discord_id,politician)` (v7) | personal watch, mirrors `watchlist` | `monitor politician:<name>` / `remove politician:<name>` | name stored lower-cased/trimmed; `politician_watchers(name)` looks up who to DM |
| `notifications(id, signal_id→signals, discord_id, message, delivered, error, created_at)` | delivery log per user | `emit`, `DiscordBot::send` callback | |

## Handy queries
```sql
-- live accuracy per stock and side
SELECT symbol, side, outcome, COUNT(*) FROM signals GROUP BY 1,2,3;
-- a user's holdings input
SELECT * FROM transactions WHERE discord_id = ? ORDER BY created_at, id;
-- learned parameters
SELECT symbol, params_json, backtest_peak_correct||'/'||backtest_peak_signals AS peak, baseline_peak FROM stock_params;
```
