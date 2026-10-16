# Changelog

All notable changes to Strojanka are listed here. Versions follow [Semantic Versioning](https://semver.org/):
MAJOR.MINOR.PATCH. The version lives in the `VERSION` file.

## [0.2.1] - Unreleased

### Added
- **Price alerts**: `/watch stock:NVDA price:250` sends one direct message when the price rises (or falls) to that level; whether it waits for a rise or a fall comes from the price when the alert is set. `/watch` alone lists your price alerts, `/unwatch stock:NVDA` removes them (`price:250` for just one). A stock with a price alert is checked even if nobody watches it. Up to 25 alerts per user. As text: `watch NVDA 250`, `watch`, `unwatch NVDA`.
- **Safety alerts** for stocks you hold: when a holding that has been comfortably in profit falls back close to your buying price, you get a warning while selling would still leave a profit after a sale fee (0.20 in the stock's currency by default, so the warning comes before the last ~20 cents of profit are gone). The warning line is about two normal days' moves of that stock above break-even (between 1.5% and 8%). It only comes while the price is falling, says it's your call whether to sell, comes once per fall and again only after the price has climbed back up. A drop straight past the line between two checks gets a "dropped below your buying price" message instead. `/safety` shows each holding's break-even and warning price; `/safety fee:0.50` sets your fee; `/safety alerts:off` silences them, `/safety stock:NVDA alerts:off` only for one stock. As text: `safety`, `safety fee 0.50`, `safety off`, `safety off NVDA`.
- Direct messages that can't be delivered (DMs turned off) now fall back to the channel you last used a command in, for these alerts and admin warnings too, like PEAK/SUPPORT alerts already did.

Database: schema 8 (`price_alerts`, `safety_state`, `user_settings`), upgraded automatically with a backup first.

## [0.2.0] - 2026-10-16

### Added
- **Backups**: a Backup button in the server window (also `backup` in its console, `admin backup` in Discord and `strojanka --backup`) saves the database as a checked `.strojanka_backup` file in `backup_folder` (default: `backups` in the install folder). The setup can start from one: *Restore a backup* under Database (`--restore <file>` in text mode); a database already there is kept beside it.
- **Backup reminders**: after 3 weeks without a backup, admins get a Discord message and, if an address is set, an email; then again every 2 days until a backup is made. The setup asks for the address and its app password (Gmail); `admin email <address>|test|off` changes it later and `strojanka --test-email` checks the mail settings. Mail goes through the user's own mailbox (SMTP over TLS), so there is no cost. The admin panel and the server window show when the last backup was made.
- **Markets**: `/market` lists 32 stock exchanges with the ticker ending to use there (`.L`, `.DE`, `.BD`...), today's or the next trading hours in this computer's time, and whether each is open now; `/market name:London` for one. The admin panel shows the markets of watched stocks, and the server window has a Markets tab. Hours follow each exchange's regular session with daylight saving time (US, EU, Sydney) and lunch breaks; public holidays aren't included.
- Prices of a watched stock are no longer fetched while its market is closed: one check after the close gets the final prices, then nothing until it opens (the console's `check` still checks everything). When every watched stock's market is closed, the server window shows *Markets closed* with the market that opens first instead of an ever older "last price check", the admin panel says the same, and the log says it once. Checking starts again right after that market opens, without waiting for the next regular check, and adding or removing a stock recalculates this at once: a newly watched stock whose market opens sooner moves the next check earlier.
- **Revert update**: every update keeps the version it replaces for 3 days (`previous` folder in the install folder), and the setup offers *Revert update* during that time, also through Settings > Apps > Strojanka > Modify, or `--revert` in text mode. Settings, watch lists and trades stay; if the update upgraded the database, the copy saved just before it is put back and the newer one is kept beside it. The copy is deleted after the 3 days.
- Politician trades from the **US House of Representatives**, read from the House Clerk's official site: a yearly list of filings and each trade report as a PDF, read by a small built-in PDF text reader (including the ~15% of reports that are RC4-"encrypted" with an empty password). The first live check read 74 filings with 664 trades from the last 60 days; only scanned paper filings are skipped. `/politicians` lists the newest filings first with owner, amount, trade and filing dates.

### Changed
- PEAK and SUPPORT alerts name the company too: "**FCX** (Freeport-McMoRan) has reached PEAK. …".
- A new politician filing is posted as one message listing its trades (up to ten, then "and N more"), instead of one message per trade; a single House report can list 57 trades.
- The first check of each chamber stores what it finds without posting it, so starting the tracker doesn't flood the channel with two months of filings.
- Watched-stock tables show the company name with the ticker, shortened for display ("NVIDIA" for NVIDIA Corporation, "JPMorgan Chase" for JPMorgan Chase & Co.): under the ticker in the server window's Watched stocks tab (full name on hover), and as a column in the console's `stocks` command and in `/status` in Discord. Long alerts in the server table wrap instead of being cut off.

### Fixed
- The politician tracker found nothing (#4): the Senate's site refuses connections from outside the US (HTTP 403), whatever the request looks like. The House is now read instead, the Senate is still tried daily, and one blocked source no longer means no data.
- `monitor politician:Pelosi` never sent a message: watches had to match the filer's full name exactly. Now every word typed must appear in the name, so a last name is enough.
- `/monitor stock:OTP` and `stock:DHL` failed with "unexpected market data format": Yahoo answers those plain tickers (empty listings) with `null` fields. They are now read safely, and a listing without prices counts as not found, so the reply suggests the real ones (`OTP.BD` in Budapest, `DHL.DE` on Xetra).
- `/monitor stock:HEAL.G` (a wrong exchange ending) gave no useful suggestion: Yahoo's search finds nothing for it, and searching `HEAL` only lists the US fund. A ticker that isn't found is now also tried on the main exchanges (London, Xetra, Amsterdam, Paris, Milan, Swiss, Budapest, US), so the reply lists `HEAL.L` (London) and the other real listings, and says when an ending like `.G` doesn't exist.
- The console's `stocks` table and the arena tables were misaligned for euro and pound amounts (padding counted bytes).

## [0.1.1] - 2026-10-14

### Added
- Politician stock trade tracking: a daily background check for new US Senate stock trade disclosures (Periodic Transaction Reports), read directly from the government's own electronic filing site since the free community trackers that used to cover this (HouseStockWatcher, SenateStockWatcher) have shut down. Senate electronic filings only for now; the House is not covered (it only publishes PDFs, which needs separate work). New trades are posted to the same public channel as the arena bots (`admin channel`). `/politicians` (also `/politicians name:Pelosi`) lists recent disclosures; `monitor politician:<name>` / `remove politician:<name>` add or remove a personal watch that sends you a direct message on a new trade. `strojanka --politicians [name]` runs a check from the command line. Schema version 7 adds the trade and watchlist tables.
- A crash handler: on an access violation, an uncaught exception reaching main(), or an abort, `strojanka-crash.log` (and a `.dmp` file on Windows) is written next to the program before it ends, so a crash leaves a trace instead of disappearing silently. Wired into all three executables. Release builds now also emit `.pdb` debug symbol files, so a `.dmp` can actually be matched to a function and source line. Crash lines are also copied into `strojanka.log`.
- The log now says how a run ended. Windows shutting down, restarting or installing an update, signing out, closing the console window and Ctrl+C are logged with the reason and stop the bot cleanly; before, each of these ended it without a word, which looked exactly like a crash. Sleep and wake-up are logged too. Every start logs when the computer last started, and warns if the previous run never finished stopping.

- Start with Windows: the installer (also when updating) offers to start Strojanka Server minimized whenever you sign in to Windows (a per-user Run entry, listed in Task Manager > Startup apps; `--autostart` / `--no-autostart` in text mode). Started that way, it waits up to three minutes for the network before connecting. Updates now keep your Start menu/desktop shortcut and start-up choices instead of resetting them, and close a running Strojanka Server cleanly instead of refusing. Opening the shortcut while Strojanka Server already runs shows that window instead of an "already running" error.
- `/portfolio`: current holdings with shares, average cost, price, value, weight and profit, totals per currency. `/history` (also `/history stock:FCX`): every recorded buy and sell, newest first, with the profit of each sale and, for one stock, the shares held after each trade.
- ETFs: monitoring, reports, alerts and `/buy`/`/sell` work for ETFs as for stocks, including European listings (`CSPX.L`, `SXR8.DE`, `IWDA.AS`). Reports mark ETFs; news is not used for them (headlines naming a fund are mostly about the fund company). A ticker that isn't found now gets a "did you mean" list from Yahoo's symbol search (`CSPX` → `CSPX.L` London, `CSPX.AS` Amsterdam; `NVIDIA` → `NVDA`). `/status` and `/profit` show one total per currency instead of adding up different currencies. Amounts like `1.234,50€` are understood.

### Fixed
- Prices quoted in pence (`GBp`, most London listings), South African cents or Israeli agorot were shown and valued as pounds, rand or shekels, 100 times too high. They are now converted to the main unit; history already stored in the minor unit is reloaded automatically, and open alerts priced in it are retired.
- An update without the settings page removed a desktop shortcut you had chosen (it always re-created the default shortcuts).
- If connecting to Discord failed with an exception (for example no network), the server window could stay on "starting"; it now shows the error with Try again.
- Tables in `/status` and `/profit` were misaligned for euro and pound amounts (padding counted bytes, and `€` is three).
- `0,858291457` (decimal comma, as in Hungary and most of Europe) was read as 858291457 shares, and `1,2345` as 12345. A comma now only groups thousands in `1,234`.
- A real crash in Pythia's training (`Gbdt::train`): a reference into a vector was read after calls that could reallocate the vector's storage, a use-after-free that mostly didn't show up in quiet testing but reliably crashed the full server, where many background services allocate memory at the same time. Found and fixed using the new crash handler's first real report.
- Uninstalling with "remove my data" only deleted `*.log` files, so a crash dump (`strojanka-crash.dmp`) was never cleaned up. Now matches the whole `strojanka-crash.*` family.
- On Windows, an unhandled exception on a background thread ended the program with no crash log at all: the crash handler's `std::set_terminate` only covers the thread that installs it. Such deaths are now caught (process-wide `SIGABRT` handler) and recorded like any other crash, and the background loops (prices, screener, arena, politicians) no longer let any exception escape.
- An incremental build could keep embedding the old version number after bumping `VERSION`, since CMake wasn't told to reconfigure when that file changes (only a full reconfigure or clean rebuild picked it up).

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
