# Architecture

## Runtime picture
Two front-ends host the same `BotHost` (src/bot/bot_host.cpp):
- `strojanka.exe` (src/bot/main.cpp `run_bot`): `host.start()` then `host.wait()` (console, no window).
- `StrojankaServer` (src/server/main.cpp): if a window titled "Strojanka Server" from the same exe exists, it is
  restored/focused and the new process exits (`activate_running_copy`). `--autostart` (the Run entry): window shown
  minimized without focus, `host.set_network_wait(3 min)` (start() retries `check_discord_token` while it returns 0;
  `request_stop()` calls `abort_network_wait()` before queuing the stop, since start holds the lock on the same pool).
  Webview window; `host.start/stop` on a 1-thread `lifecycle` pool under a
  `shared_mutex` (unique for start/stop, shared for commands/status; status uses try_lock so the UI never blocks);
  console commands on a 2-thread pool resolving the page's promise (`w.bind` async + `w.resolve`); log lines via
  `log_set_listener` â†’ `LogFeed` (1000 recent + pending) â†’ flusher thread every 150 ms â†’ `window.__logs([...])`.
  JS bindings: `serverInfo, recentLogs, serverState (state/error/stats JSON), serverStocks, startBot, stopBot,
  runCommand(line), openPath(config|folder|log)`. Closing the window â†’ `host.stop()` (screener cancels promptly).

```
BotHost::start (src/bot/bot_host.cpp)
 â”śâ”€ load_config(strojanka.conf) â”€ resolve_config_path() for db/log paths; log level/file
 â”śâ”€ check_discord_token()  (curl GET /users/@me, 401 -> fail)
 â”śâ”€ InstanceLock("<db>.lock")  (exclusive; second copy fails with a clear message)
 â”śâ”€ Database + migrate_database(backup=true)
 â”śâ”€ Repository, YahooProvider, news::RssNewsProvider
 â”śâ”€ MonitorService, Screener, CommandService, AdminService (commands.set_admin), ServerConsole
 â”śâ”€ HouseClerkClient + SenateEfdClient, PoliticianService (set_politicians on ServerConsole)
 â””â”€ DiscordBot(..., politicians) : Notifier   â†’ start() = cluster.start(st_return); online()/name() for the UI
      on_ready (once): register global slash commands with contexts {itc_bot_dm} (DM-only product
                       decision; legacy guild_id -> delete that guild's commands), set notifier, monitor.start(),
                       politicians.start() (announcer â†’ post_to_public_channel, notifier â†’ this)
      on_slashcommand: thinking(ephemeral) -> ThreadPool(4) -> CommandService -> interaction_response_edit
      on_message_create: DMs only (guild_id == 0, not bots) -> ThreadPool -> CommandService::run_text -> message_create
      Notifier::send: DM; on failure post "<@user> msg" in users.notify_channel_id
```

### Threads
| Thread | Owner | Work |
|---|---|---|
| D++ internal threads | `dpp::cluster` | gateway, REST, event dispatch |
| command workers (4) | `DiscordBot::workers_` | run `CommandService` methods (may download prices) |
| monitor loop (1) | `MonitorService::thread_` | `run_cycle()` now, then every `poll_interval_minutes` (condition_variable wait, `stop()` wakes it) |
| cycle pool (`max_parallel_fetches`) | created per cycle in `run_cycle()` | `refresh(symbol, emit_signals=true)` per distinct watched symbol |
| arena loop (1) + pool | `ArenaService::thread_` (started on ready) | hourly `tick(last_complete_date(now))`: refresh universe prices (10y first, then 1mo every 6 h), rebuild `Universe`, Pythia `train` (when backtests are due or no model; ~30 s CPU) then `annotate`, weekly backtests, advance live bots day by day, promote/demote, weekly leaderboard; announcements via `set_announcer` â†’ `DiscordBot::post_to_public_channel` (channel id in `app_meta.arena_channel_id` / `AdminService::kPublicChannelKey`) |
| D++ timer (hourly) | `DiscordBot` (on ready) | submits `AdminService::check_health(*this)` to the command workers; also once right after owners load |
| screener loop (1) + pool | `Screener::thread_` (started on ready) | wakes hourly or on `request_run()`; if `app_meta.screener_last_run` > 20 h old: `run_now()` = fetch 10y + `walk_forward` + current state for each universe symbol (~15 s for 70) |
| session watcher (1) | `SessionWatch` (front-ends, while the bot runs) | message loop of the hidden window; console ctrl handler runs on a system thread |
| politicians loop (1) | `PoliticianService::thread_` (started on ready) | once daily (`app_meta.politicians_last_run` > 24 h old): `tick()` = `list_filings` over the last 60 days, skip paper filings and already-seen `report_id`s, `fetch_filing` each new one, store via `add_politician_trades`, announce new trades via `set_announcer` â†’ `post_to_public_channel`, and DM each trade's `politician_watchers` via `set_notifier` |

Concurrency rules: SQLite connection is shared and serialized by `Database::mutex()` (recursive)
held by each `Repository` call. `MonitorService::symbol_mutex(symbol)` serializes refreshes of
the same symbol (cycle vs. a user's `/monitor`). Heavy math runs outside the DB lock.

### refresh(symbol) pipeline (`src/core/monitor_service.cpp`)
1. `full = history_full_refresh_at older than 20h` â†’ fetch `TenYears`, else `Month`; `upsert_bars`. If the stored `stocks.currency` is a minor unit (pre-fix GBp rows): full fetch + `replace_bars` + pending signals â†’ EXPIRED.
2. `load_bars(symbol, 520)` (analysis window). Fewer than 30 bars â†’ error.
3. Grade pending signals: `evaluate_outcome` from the signal's bar; resolve CORRECT/WRONG/EXPIRED.
4. Learn: `adjust_bias` per side if something resolved; `calibrate()` if no params / full refresh / resolved / >20h. On full refresh or stale backtest: `walk_forward(load_bars(2600), 520)` â†’ `stock_params.backtest_*`, `baseline_*`.
5. `analyze_series` on the window â†’ last `BarState`; `classify` (category), `short_term_trend`; `upsert_stock`.
5b. News (skipped entirely for funds: `ChartData::is_fund()`, summary empty, report/alerts say so; otherwise if a `NewsProvider` was given and `stocks.news_fetched_at` > 1 h old): fetch â†’ `add_news`; then `summarize(last 72 h)`; adjusted `peak = state.peak â’ wÂ·tone`, `support = state.support + wÂ·tone` (w = `news_weight()`), stored in `StockReport.peak_score/support_score/news`. Alerts use the adjusted scores; SUPPORT is held back when the summary has red flags. `run_cycle` ends with `learn_news_weight()` (only when the graded-with-news count changed).
6. If `emit_signals`, the last bar is â‰¤5 days old and the score â‰Ą cutoff: `emit()` â†’ 3-day per-side cooldown (`has_signal_since`) â†’ `insert_signal_if_new` (UNIQUE symbol/side/leg_key) â†’ per watcher message (PEAK only to holders) â†’ `notifications` row â†’ `Notifier::send`.

## File index
### src/common (std only; compiled into core AND installer/packer)
| File | Key symbols |
|---|---|
| strings.h/.cpp | `trim`, `to_upper`, `parse_amount` ("59.78$", "$1,234.5", "12,5"), `format_money`, `format_signed_money`, `format_shares` (â‰¤9 dp), `format_percent`, `utc_now_iso`, `unix_to_date`, `iso_to_unix` (own civil-date math, no timegm), `normalize_symbol`, `is_valid_symbol`, `short_company_name(name, max_chars)` (drops Inc./Corporation/plc/AG/"& Co."…, a leading "The" only when something was dropped; truncates with "…"), `utf8_length` (use it for table padding: € is 3 bytes) |
| config.h/.cpp | `Config` (discord_token, guild_id, database_path, poll_interval_minutes, max_parallel_fetches, log_file, log_level, installed_version), `load_config` (unknown keys ignored), `save_config` (chmod 600 on POSIX), `resolve_config_path` |
| payload.h/.cpp | installer container format (`STRJPAK1` footer, FNV-1a 64 per file), `pack`, `read_index`, `extract` (writes `.part` then rename; refuses `..`) |
| platform.h/.cpp | `executable_path` (GetModuleFileNameW / _NSGetExecutablePath / /proc), `executable_dir`, `enable_utf8_console` |
| instance_lock.h/.cpp | `InstanceLock(path)`: Windows exclusive CreateFile (share 0, delete-on-close), POSIX `flock`; `acquired()` |
| utf8_path.h | `utf8_path(std::string)`, `path_utf8(path)`, `path_utf8_generic(path)` |
| html.h/.cpp | `strojanka::html::decode_entities`, `strip_tags`, `text_of` (= strip_tags + decode_entities + trim); shared by `news_provider.cpp` and `politicians/senate_efd.cpp` |
| crash_handler.h/.cpp | `install_crash_handler()` (first line of every `main`): `SetUnhandledExceptionFilter` (access violations, any thread) + `std::set_terminate` (**per-thread on MSVC**, so it only covers main) + process-wide `SIGABRT` handler (catches terminate/abort on every other thread). Each writes `strojanka-crash.log` + `strojanka-crash.dmp` next to the exe; `set_crash_log_mirror(log)` (BotHost) also appends to strojanka.log. Never uses core/log (its mutex could deadlock). Not catchable: `__fastfail` (heap corruption, /GS), stack overflow, external kill. |
| platform.h/.cpp | also `system_boot_unix()` (GetTickCount64 / KERN_BOOTTIME / sysinfo) |

### src/core
| File | Key symbols |
|---|---|
| version.h.in | `STROJANKA_VERSION` (generated to build/.../generated/core/version.h) |
| log.h/.cpp | `log_set_level`, `log_set_file`, `log_write`, `parse_log_level` |
| database.h/.cpp | `Database` (opens with FULLMUTEX, WAL, foreign_keys, busy_timeout 5s), `Statement` (1-based bind, 0-based columns), `SqlTransaction` (BEGIN IMMEDIATE, rollback in dtor), `DatabaseError` |
| migrations.h/.cpp | `all_migrations()`, `latest_schema_version()`, `migrate_database(db, make_backup)` (VACUUM INTO backup; refuses newer schema) |
| models.h | `Bar`, `TransactionRecord`, `StockRecord`, `SignalRecord`, `StockParamsRecord`, enums `TxSide`, `SignalSide`, `SignalOutcome` |
| repository.h/.cpp | all SQL; users, watchlist, stocks, price_history, transactions, stock_params, signals, notifications |
| portfolio.h/.cpp | `Position` (shares, cost_basis, realized_profit, ...), `apply_transaction(pos, tx)` (the single average-cost step), `compute_position(txs)`, `trade_history(txs)` → `TradeEntry{tx, shares (sale capped at held), price, realized_profit, held_after}` replaying any number of symbols |
| thread_pool.h | header-only `ThreadPool::submit` â†’ `std::future` |
| market/market_data.h | `MarketDataProvider` interface (`fetch`, optional `search` → `SymbolMatch{symbol,name,type,exchange}`, default empty), `ChartData` (+`instrument_type`, `is_fund()`; `currency` is always a major unit), `HistoryRange {Month, TwoYears, TenYears}`, `major_currency` (GBp/GBX→GBP, ZAc→ZAR, ILA→ILS, ÷100) |
| market/yahoo_provider.h/.cpp | `YahooProvider` (curl, `query1.finance.yahoo.com/v8/finance/chart`; `search` = `query2.finance.yahoo.com/v1/finance/search`, EQUITY/ETF only), `parse_yahoo_chart` (adjclose-adjusted OHLC, null bars skipped, minor-unit prices ÷100, `meta.instrumentType`), `parse_yahoo_search` |
| analysis/indicators.* | `rsi` (Wilder 14), `percent_b` (20, 2Ď), `daily_volatility`, `log_price_regression`, `median`, `smoothstep`, `clamp01` |
| analysis/zigzag.* | online `ZigZag(reversal_pct)` (`direction()`, `pivots()`, `extreme()`), `zigzag_pivots` |
| analysis/analyzer.* | `AnalysisParams` (+json, `peak_cutoff()`), `Indicators`, `BarState`, `analyze_series`, `evaluate_outcome` |
| analysis/calibrator.* | `default_params`, `calibrate` (grid search), `adjust_bias`, `SideStats`, `CalibrationResult` |
| analysis/walk_forward.* | `walk_forward(bars, train_window, step)` â†’ `WalkForwardResult` incl. random-day baselines |
| analysis/classifier.* | `StockCategory`, `classify`, `category_name/description`, `Trend`, `short_term_trend` |
| monitor_service.h/.cpp | `Notifier`, `StockReport`, `MonitorService` (`start/stop/run_cycle/refresh/emit`) |
| messages.h/.cpp | `peak_alert`, `support_alert`, `format_report`, `has_proven_edge` (â‰Ą8 calls and â‰Ą baseline+5pp) |
| archive.h/.cpp | `inflate_data(bytes, raw)` (zlib), `zip_entry(zip, name_suffix)` (stored/deflate, central directory; no ZIP64) |
| pdf_text.h/.cpp | `pdf_text_fragments(pdf)` → `PdfText{y,x,text}`: objects found by scanning (`/Length` or `endstream`), FlateDecode, fonts gathered file-wide by resource name with ToUnicode CMaps (bfchar, bfrange incl. arrays; EO.Pdf small caps 0x283..0x29C → A..Z), operators Tf/Tm/Td/TD/T*/Tj/TJ/'/" (a run ends at Tm or a vertical move), inline images skipped; standard security handler V1/V2 R2/R3 (RC4, empty user password) via `pdf_crypto` |
| pdf_crypto.h/.cpp | `md5` (RFC 1321, K table computed and checked by test vectors), `rc4` |
| http.h/.cpp | `http_get(url)` â†’ `HttpResponse{ok,status,body,error}` (libcurl, shared by prices and news), `url_encode` |
| news/news_provider.h/.cpp | `news::NewsProvider` interface; `RssNewsProvider` = Yahoo `feeds.finance.yahoo.com/rss/2.0/headline?s=SYM` + Google News RSS search `"<company>" stock`; keeps â‰¤14-day items that name the ticker or company's first word; strips Google's " - Publisher"; dedup by `title_key`. `parse_rss` (CDATA/entities), `parse_rfc822`, `search_name` |
| news/sentiment.h/.cpp | `score_headline` (phrase+word lexicon Â±1/Â±2, negators within 3 tokens flip, split analyst target pattern, red-flag terms â’3 + flag, `?` Ă—0.5, clamp Â±1), `summarize` (72 h window, weight 0.5^(age/24h), shrink Ă—n/(n+2)), `describe`, `adjust_news_weight` (â‰Ą10 agree & â‰Ą10 disagree graded alerts with \|tone\|â‰Ą0.1: diff>10pp â†’ +0.01, diff<0 â†’ â’0.01, range 0..0.15), `kDefaultNewsWeight` 0.05 |
| screener.h/.cpp | `Screener` (`start/stop/request_run/run_now/screen_one/universe`, `default_universe()` â‰70 US large caps or `config.screener_symbols`), `conservative_edge(correct, n, baseline)` = Wilson(z=1.645) lower bound â’ baseline, â’1 if n<8. Results do not touch `price_history`/`stocks`. |
| arena/engine.h/.cpp | `Spec` + `roster()` (13 bots: Atlas BuyHold; Pythia/Sibylla Oracle (ml::Pythia up-chance ≥ .50/.55, hold ≤20 days, -8% stop, exit when chance < c-0.1); Athena/Minerva Swing c=.65/.75; Ares/Mars Trend 20/100, 50/200; Hermes/Mercurius Momentum 126/252 (12-1); Artemis/Diana Breakout 20/10, 55/20; Nemesis/Iustitia MeanReversion RSI2<10 exit>SMA5, RSI2<5 exit RSI2>70), `build_universe` (calendar = days â‰Ą half the symbols traded; indicators per symbol; swing params from the first year only), `step` (fill pending orders at today's open Â±0.1% slippage, sells first; size = min(cash/remaining buys, equity/max positions); value at close; decide), `measure`, `backtest` (+ gate: â‰Ą10 sells, Sharpe â‰Ą0.6, max DD â‰¤35%, every third >0) |
| arena/arena_service.h/.cpp | `ArenaService`: stages training â†’ proving (40 live days, â‰Ą Atlasâ’2pp, DD â‰¤15%, rolling window) â†’ public (demote: DD >25% from high, or 60-day lag >10pp vs Atlas, or weekly backtest fails); `tick`, `view`, `leaderboard(include_private)`, `details`, `channel_id`/`set_channel_id`, `last_complete_date` (21:30 UTC) |
| ml/gbdt.h/.cpp | `Gbdt`: logistic gradient-boosted trees (32 quantile bins, NaN bin, depth 3, 120 trees, lr 0.06, min_leaf 150, L2 10, row 0.5/feature 0.8 sampling), `predict`, JSON save/load; `auc()` |
| ml/pythia.h/.cpp | `Pythia(repo)`: `features()` (25, row i uses bars ≤ i; names in `feature_names()`), `market_context()` (per date: mean 20d/60d return, share above SMA200), `train(Universe&)` (labels = `evaluate_outcome` Support/Peak with move = clamp(2.5·vol20, 1.5%, 15%), horizon 20; yearly walk-forward from min_year+3 with a 35-day gap, writes OOS preds into `SymbolData.oracle_up/down` for whole test years; final models on data up to last−35 d; report AUC/base/top-decile; `save_model("pythia")` incl. last 400 days of context), `annotate` (fills NaN in the last 300 bars with final models; latest picks), `predict(symbol, bars)`, `load`, `status_text` |
| politicians/disclosure_source.h/.cpp | `DisclosureSource` interface (`chamber()`, `start`, `list_filings(start,end)` → `FilingRef{first_name,last_name,office,filed_date,report_id,url,electronic}`, `fetch_filing(ref)` → `DisclosedTrade{report_id,item_number,transaction_date,owner,ticker,asset_name,asset_type,transaction_type,amount_min,amount_max(optional),comment}`); `normalize_date` (MM/DD/YYYY, M/D/YYYY), `parse_amount_range` |
| politicians/senate_efd.h/.cpp | `SenateEfdClient` (chamber Senate) = PIMPL'd libcurl session (cookie jar for the CSRF token, `CURLINFO_COOKIELIST`); `parse_transaction_table(html, report_id)` with the `find_blocks()` tag matcher. Akamai answers 403 to this from outside the US (issue #4: Hungarian home connection, browser headers and SChannel made no difference). |
| politicians/house_clerk.h/.cpp | `HouseClerkClient` (chamber House): `list_filings` downloads `public_disc/financial-pdfs/<year>FD.zip` per year in range, `zip_entry(..., "FD.txt")`, `parse_house_index` (tab-separated, Windows-1252, FilingType P, DocID starting 2 = electronic, office "Representative for XX00"); `fetch_filing` downloads `ptr-pdfs/<year>/<DocID>.pdf` (300 ms apart) → `parse_house_ptr`: `pdf_text_fragments`, sorted by (y, x), table below y 330 until "* For the complete" or an all-caps heading at x<10; a row starts at each MM/DD/YYYY in the Date column (x 400-475); columns Owner 50 / Asset 100 / Type 315 / Date 400 / Notified 475 / Amount 560 / Gains 665; FILING STATUS / SUBHOLDING OF / DESCRIPTION / COMMENTS label lines (x<115) end the asset name. Codes mapped: SP Spouse, JT Joint, DC Dependent Child, blank Self; P Purchase, S Sale (Full), S (partial) Sale (Partial), E Exchange. |
| politicians/politician_service.h/.cpp | `PoliticianService(repo, vector<DisclosureSource*>, Config)`: `start/stop`, `tick()` (daily, `kLookbackDays`=60, last run in `app_meta.politicians_last_run`) → `check(source)` per source (a failing one is logged, the rest continue): list → skip paper and already-stored `report_id`s → fetch → `add_politician_trades`; the first check of a chamber (`has_politician_trades_in` false) stores quietly; afterwards `announce_filing` = one `format_filing_announcement` message per filing (≤10 lines, then "...and N more") to the announcer and to `politician_watchers_for(full_name)` (every watched word must be a word of the name) |
| server_console.h/.cpp | `ServerConsole::run(line)` (help, status, stocks, users, check, screen, report/news/suggest, `as <id> <cmd>` (ensure_user + run_text), admin â€¦ via `AdminService::handle_console` (trusted), `arena`/`arena run`/`arena channel`, `politicians`/`politicians run`/`politicians channel` (shares `kPublicChannelKey`) via `set_politicians`, loglevel, version), `watched_stocks()` rows for the UI table |
| admin_service.h/.cpp | `AdminService(db, repo, monitor, config, config_path)`: `set_owners`, `is_admin`, `admins`, `handle(user, args)` (status/list/add/remove/movedb), `status_text`, `move_database(folder)` (space check â‰Ą1.5Ă—size+50MB, `db.move_to`, rewrite conf), `check_health(Notifier&)` (disk < `low_disk_warning_mb` â†’ DM all admins, 24 h throttle; `CycleStats.failed_cycles_in_a_row â‰Ą 2` â†’ outage DM, 6 h throttle). `parse_user_id`, `format_bytes`. |
| run_state.h/.cpp | `kRunStateKey`/`kRunStartedKey` (app_meta: `running` / `stopped` / `ended: <reason>`), `describe_previous_run(state, started, boot)` → `PreviousRun{warn,text}` logged by BotHost at start |
| command_reply.h | `CommandReply{ok,text}` (shared by commands and admin) |
| commands.h/.cpp | `CommandService` (`help,status,profit,monitor,remove,buy,sell,report,monitor_politician,remove_politician,politicians`), `CommandReply{ok,text}`; `run_text` dispatches `monitor politician <name>` / `remove politician <name>` / `politicians <name filter>`; `lookup_failed` adds "did you mean" matches (≤5, via `MonitorService::search_symbols`) when the error says not found; `/status` and `/profit` total per currency (no FX conversion); `portfolio` (holdings, weight per currency) and `history` (newest first, rows added until `kReplyBudget` 1900 chars so the code block never gets cut by `fit_message`); `pad` counts UTF-8 characters |

### src/bot
| File | Key symbols |
|---|---|
| main.cpp | CLI: `--config`, `--migrate`, `--analyze SYM`, `--news SYM`, `--suggest [side]`, `--move-db DIR`, `--backtest SYM...`, `--version`; `default_config_path` (exe dir, then cwd); `run_bot` = BotHost start+wait |
| bot_host.h/.cpp | `BotHost(config_path)`: `start(&error)` (logs boot time + `describe_previous_run`, sets run_state `running`), `stop()` (sets `stopped`, logs "stopping Strojanka" … "Strojanka stopped"), `wait()`, `request_stop()` (wakes wait() only), `note_session_end(reason)`, `running()`, `online()`, `bot_name()`, accessors for db/repo/monitor/screener/admin/console/politicians; `check_discord_token` |
| session_watch.h/.cpp | `SessionWatch(on_end(reason, forced))`: hidden top-level window on its own thread (message-only windows miss the broadcasts) for WM_QUERYENDSESSION/WM_ENDSESSION (shutdown, restart, update/Restart Manager, sign-out) and WM_POWERBROADCAST (logs sleep/wake), plus `SetConsoleCtrlHandler` (Ctrl+C = not forced; console close/logoff/shutdown = forced). Logs the reason, calls on_end once; forced → front-end stops within `kSessionEndGrace` (4 s) because Windows ends the process right after. Front-ends: `run_bot` (request_stop → main thread stop()), server (`lifecycle.submit(stop_bot).wait_for`). No-op off Windows. |
| tls_trust.h/.cpp | `configure_tls_trust(pem_path)`: vcpkg OpenSSL has no OS root CAs, so D++'s TLS verify fails (D++ reports it only as "Malformed HTTP response"). Unless `SSL_CERT_FILE` is set: Windows exports the ROOT stores to `<db dir>/trusted_roots.pem`; macOS uses `/etc/ssl/cert.pem`. Runs before `dpp::cluster` is created. |
| discord_bot.h/.cpp | `DiscordBot` (ctor also takes `politicians::PoliticianService&`; `register_commands` incl. `/politicians name:`, `/monitor` and `/remove` with optional `stock`+`politician` (`dispatch` requires exactly one)), `on_command`, `dispatch`, `send`, `post_to_public_channel` (renamed from `post_to_arena_channel`, now used by both arena and politicians announcers), `fit_message` (2000-char limit, UTF-8 safe) |

### src/installer
| File | Key symbols |
|---|---|
| main.cpp | arg parsing; GUI unless `--console`/`--yes`; GUI failure (rc â’1, e.g. no WebView2) â†’ `attach_console()` + console. Windows build is a GUI-subsystem exe with `/ENTRY:mainCRTStartup`, so console mode attaches to the parent console or allocates one. |
| installer_core.h/.cpp | `inspect_install(dir)` â†’ `ExistingInstall` (found, version, config, suggested: install/update/repair/downgrade, current start_menu/desktop/autostart so updates keep them); `execute(Request, container, ProgressFn)` â†’ `Result`. Install/Update/Repair/Uninstall: `stop_running_copy` closes a running server window (90 s), refuses if still locked (e.g. console copy) â†’ settings (keep or new) â†’ extract only entries whose size/FNV hash differ (Repair reports `files_written`) â†’ launcher â†’ `save_config` â†’ `strojanka --migrate` via `os::run_capture` â†’ copy self to `StrojankaSetup.exe` â†’ shortcuts â†’ `register_app`. Uninstall: removes payload-listed files, launcher, shortcuts, registry; `remove_data` deletes config/logs and DB files **only inside** the install dir; `prune_empty` (never deletes non-empty dirs); a running `StrojankaSetup.exe` is deleted by `os::delete_after_exit`. |
| platform_install.h/.cpp | `os::` Windows: `file_in_use` (exclusive open â†’ sharing violation), `run_capture` (pipe, CREATE_NO_WINDOW), `pick_folder/pick_file` (IFileOpenDialog), shortcuts (IShellLinkW in FOLDERID_Programs/Desktop), `register_app` (HKCU `...\Uninstall\Strojanka`, ModifyPath = setup, UninstallString = setup `--uninstall`), `current_integration()` (which shortcuts / Run value exist), `set_autostart(dir, on)` (HKCU `...\CurrentVersion\Run` value `Strojanka` = `"<dir>\StrojankaServer.exe" --autostart`), `close_running_server(dir, s)` (WM_CLOSE to top-level windows titled "Strojanka Server" whose process image is that exe, then waits for the file lock). macOS: popen/osascript/`open`; shortcuts/registry are no-ops. |
| gui.h/.cpp | `run_gui`: `webview::webview` 880Ă—600 fixed; JS bindings `appInfo`, `inspectDir`, `pickFolder`, `pickFile`, `openPath`, `closeWindow`, `startTask(request)`; the task runs on a worker thread and reports via `w.dispatch` â†’ `window.__progress(f, msg)` / `window.__finished(result)`. |
| ui/installer.html | whole interface (pages welcome/settings/uninstall/progress/done, light+dark CSS, inline SVG logo). Embedded at build as `kInstallerHtml` (`generated/installer/installer_ui.h` via `cmake/embed_file.cmake`). |
| console_ui.h/.cpp | `run_console(container, ConsoleOptions)`: text questions or `--yes` defaults, then the same `execute()`. |

Flags: `--console --yes --dir --token --new-db --existing-db --poll --keep-config --uninstall --remove-data`.
With `--yes`, an existing config and no `--token`: Update/Repair keeping settings.

### Build/packaging (CMakeLists.txt)
`install()` rules define the payload: bot exe, all `*.dll` from `build/.../bin` (vcpkg app-local deploy), MSVC runtime via `InstallRequiredSystemLibraries`, VERSION, CHANGELOG.md, `documentation/` â†’ `docs/`.
Windows exes get `src/common/app.rc.in` (icon as resource 32512 = IDI_APPLICATION, which webview loads for the window; VERSIONINFO).
Target `dist`: rm stage â†’ `cmake --install --prefix build/.../stage` â†’ `strojanka_pack <installer> <stage> dist/StrojankaInstaller.exe`.
macOS: the installer is a `MACOSX_BUNDLE` (icns, `-framework Cocoa -framework WebKit`); `dist` copies the bundle to `dist/StrojankaInstaller.app` and packs into `Contents/MacOS/StrojankaInstaller` (untested on a Mac).
