# Recipes

## Add a slash command
1. `src/core/commands.h/.cpp`: add `CommandReply foo(const std::string& user, ...)`. Validate input with `parse_symbol` / `parse_amount`; return `{false, text}` for user errors.
2. `src/bot/discord_bot.cpp`: in `register_commands()` add a `dpp::slashcommand` with options; in `on_command()` read options with `param("name")` (strings only); route in `dispatch()`.
3. Update `CommandService::help()`, `documentation/commands.md`, and add a test in `test_commands_and_alerts`.
Slash commands are re-registered on every start (bulk overwrite), so no manual cleanup is needed.

## Change the database schema
1. Append a migration in `src/core/migrations.cpp` (`{2, "what changed", R"SQL(ALTER TABLE ...;)SQL"}`).
2. Update `models.h` structs and `Repository` SQL.
3. Update `agent_documentation/database.md`, add a CHANGELOG entry, and extend `test_migrations` (e.g. create a v1 DB, insert rows, migrate, assert data survives).

## Add a market data source
Implement `MarketDataProvider::fetch(symbol, range, error)` (thread-safe; return adjusted daily bars oldest-first),
then choose it in `src/bot/main.cpp` (`run_bot`, `run_analyze`, `run_backtest`). For a config switch add a key (below).

## Add a config key
`src/common/config.h` field with a default → parse in `load_config` → write in `save_config` (with a comment line)
→ optionally ask in `src/installer/main.cpp` and add an unattended flag → document in `documentation/installing.md`.
Unknown keys are ignored by old versions, so adding keys is backward compatible.

## Improve the signal model
1. Change `analyze_series` / `calibrate` (keep no-look-ahead).
2. Measure: `strojanka --backtest <train set>` then the holdout set listed in `analysis_engine.md` §8. Compare PEAK/SUPPORT hit rate against the `random` column; only keep changes that improve the holdout.
3. Unit tests: `test_calibration_learns_cycles` must keep passing (synthetic cycle sanity).
4. If `AnalysisParams` gains fields, add them to `to_json/from_json` with defaults (stored JSON from older versions must still load).

## Add a scraped external data source (pattern used by politicians)
Define a small abstract interface in `src/core/<feature>/` (mirrors `MarketDataProvider`/`SenateSource`) so a
background `*Service` can be unit-tested against a fake implementation with zero real network calls. Keep
HTML/text parsing in a free function exposed for tests (e.g. `parse_transaction_table`), separate from the
networking class. Document any bot-protection/rate-limit risk honestly in code comments and user docs rather
than guessing it away — test real endpoint behavior with `curl` before writing parser code.

## Add another front-end (web, Telegram, CLI)
Create a new executable target linking `strojanka_core`; construct `Database/Repository/YahooProvider/MonitorService/CommandService`
as in `run_bot`; implement `Notifier::send` (must call `repo.mark_notification`). Do not put platform types into core.

## Change the installer's look or steps
Edit `src/installer/ui/installer.html` (rebuild re-embeds it). New C++ calls: add `w.bind("name", ...)` in `gui.cpp`
(argument = JSON array string, return = JSON). Install logic belongs in `installer_core.cpp` so the console mode gets it too.
Visual check without clicking: launch `dist\StrojankaInstaller.exe --dir <scratch>`, screenshot the window
(`DwmGetWindowAttribute` + `CopyFromScreen`); `SendKeys` `{ENTER}` advances pages (Enter = Next).

## Change the icon
Edit `assets/icon/strojanka.svg` **and** the same shapes in `tools/make_icons.ps1`, run the script, commit the generated
`.ico/.icns/png`. Also update the inline SVG logo in `src/installer/ui/installer.html`.

## Add an arena bot
1. `src/core/arena/engine.h`: new `Kind` if needed. `engine.cpp`: add the `Spec` to `roster()` (Greek or Latin name, `origin`, one-sentence `idea`, params) and its rules in `enters` / `keeps` / `score` (or `decide` for cross-sectional strategies). Use only data at bar index ≤ today.
2. Precompute any new indicator in `build_universe` (`SymbolData`).
3. The bot appears automatically (ensure_bots), starts in training and must pass the history test and 40 live days.
4. Run `strojanka --arena` to see its history test; keep `arena engine: accounting, no look-ahead` green.

## Release a new version
Only when the user says so (see invariant 9).
1. Bump `VERSION` (SemVer). 2. Add a `CHANGELOG.md` section. 3. `build_for_windows.bat` and `./build_for_mac.sh` → `dist/`.
The installer writes `installed_version` into `strojanka.conf`; upgrades keep config and migrate the DB with a backup.

## Debugging tips
- `log_level=debug` in `strojanka.conf` logs learned params per calibration.
- `--analyze SYMBOL` exercises fetch → calibrate → walk-forward → report with no Discord.
- Yahoo returns HTTP 404 / `chart.error` for unknown symbols → "symbol not found".
- Invalid token → `check_discord_token` exits with a clear error (D++ alone would just hang with odd errors).
