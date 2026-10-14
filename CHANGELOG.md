# Changelog

All notable changes to Strojanka are listed here. Versions follow [Semantic Versioning](https://semver.org/):
MAJOR.MINOR.PATCH. The version lives in the `VERSION` file.

## [0.1.0] - 2026-10-10

First version.

### Added
- Discord slash commands: `/help`, `/status`, `/profit`, `/monitor`, `/remove`, `/buy`, `/sell`, `/report`.
- Background monitor that refreshes every watched stock in parallel (each symbol is fetched once, however many users watch it).
- PEAK / SUPPORT detection based on swing cycles, RSI and Bollinger bands, with per-stock parameters learned by backtesting.
- Learning from live mistakes: every alert is graded later and repeated misses make the stock's thresholds stricter.
- Honest accuracy: a daily walk-forward test on 10 years of history; reports and alerts show it next to the random-day baseline. `strojanka --backtest SYMBOL...` runs the same test from the command line.
- Stock categories: cyclical, trending up, trending down, high volatility, defensive, mixed.
- Average-cost portfolio tracking with realized and unrealized profit.
- Single-file SQLite database with versioned migrations and automatic backup before upgrades (schema version 1).
- Self-extracting installer for Windows and macOS; `build_for_windows.bat` and `build_for_mac.sh`.
- Windowed setup with a modern look (light and dark mode). It detects an existing installation and offers Update, Repair (restores only missing or damaged files), Change settings or Uninstall. On Windows it adds Start menu and desktop shortcuts and an "Installed apps" entry. A text mode (`--console`, `--yes`) remains for scripts and computers without WebView2.
- Strojanka icon (`assets/icon`), used by the bot, the installer and the setup window.
- News: hourly headlines from Yahoo Finance and Google News RSS, scored with a finance word list. The last three days' tone nudges PEAK/SUPPORT scores by a learned weight (starts at 0.05, 0 to 0.15, learned from graded alerts), and red-flag headlines block SUPPORT alerts. New `/news` command and `strojanka --news SYMBOL`; reports and alerts show a news line. Schema version 3 stores headlines (`news_items`) and the news tone of each alert.
- Pythia, a built-in AI model (gradient-boosted decision trees written in C++, CPU only, no paid services): chances of a rise or fall within 20 trading days from 25 measurements, trained on all arena stocks, tested year by year on unseen years, retrained weekly, saved in the database (schema version 6). Shown in reports, alerts and `/pythia`; the arena bots Pythia and Sibylla trade on it. First honest test: no edge yet (ranking skill 0.51).
- Arena: paper-trading bots named after Greek and Roman gods (Atlas the buy-and-hold benchmark; Athena/Minerva swing signals; Ares/Mars trend; Hermes/Mercurius momentum; Artemis/Diana breakouts; Nemesis/Iustitia mean reversion), $1,000 of play money each. A bot must pass a test on about 8 years of past prices, then 40 trading days of private live trading, before its trades and profits are posted in a Discord channel (`admin channel <id>`); weekly re-tests and demotion rules keep it honest. Orders fill at the next day's open with 0.1% costs. `/arena` in Discord, an Arena tab and `arena` commands in the server window, `strojanka --arena`. Schema version 5 adds the arena tables.
- Strojanka Server: a window that runs the bot, with a status bar (online/offline, Start/Stop), dashboard cards (users, stocks, alerts, headlines, database, disk, last price check), a watched-stocks table, a live colour-coded log and a command line (`status`, `stocks`, `users`, `check`, `screen`, `as <user> <command>`, `admin …`, `loglevel`…). The Start menu shortcut and the installer's "Start Strojanka now" open it; `strojanka.exe` remains the windowless version.
- Only one copy of the bot can run per database (lock file next to it), so alerts are never sent twice.
- Admin panel: `/admin` (status, add, remove, movedb). The Discord application's owner is an admin automatically; schema version 4 stores further admins. Admins get direct-message warnings for low disk space on the database drive (`low_disk_warning_mb`, hourly check) and for price data outages.
- Moving the database: `admin movedb <folder>` while running, or `strojanka --move-db <folder>` while stopped. The copy is integrity-checked before the old file is deleted, and the settings file is updated.
- The daily screener also stores headlines for every stock it checks, building news history for future accuracy tests.
- `/suggest` (also `suggest buy` / `suggest sell`): a daily background screener runs the honest test on about 70 large US stocks (or `screener_symbols`) and lists those that beat the random-day baseline at 90% confidence. `strojanka --suggest` runs it from the command line. Database schema version 2 adds the `screen_results` table.
- The bot is used in its direct-message chat, with slash commands or plain text (`buy FCX 0.858291457 59.78$`).

### Known limitations
- On most stocks the PEAK/SUPPORT model does not yet beat the random-day baseline out of sample (see `documentation/how-signals-work.md`).
- The macOS build script and the macOS setup app have not been run on a Mac yet.
