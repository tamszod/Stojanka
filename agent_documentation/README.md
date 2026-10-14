# Strojanka â€” agent orientation (start here)

Discord bot (C++20, CMake, vcpkg) that watches stocks per Discord user, sends PEAK (sell) /
SUPPORT (buy) DMs, tracks buys/sells with average-cost profit. Version in `/VERSION` (0.1.1).
SQLite single file, versioned migrations. Self-extracting installer for Windows/macOS.

Read next, as needed:
- [architecture.md](architecture.md): components, threads, data flow, every file and its key symbols
- [database.md](database.md): schema v1, migration rules, which code touches which table
- [analysis_engine.md](analysis_engine.md): signal algorithm, learning, grading, measured accuracy, arena (trading bots), politicians (Senate trade tracking)
- [recipes.md](recipes.md): step-by-step changes (new command, migration, provider, config key, front-end, release)

## Layout
```
VERSION                  single source of version (CMake reads it -> build/<dir>/generated/core/version.h)
CMakeLists.txt           targets: strojanka_core (lib), strojanka (bot), StrojankaInstaller, strojanka_pack, strojanka_tests, dist
vcpkg.json               deps: dpp, sqlite3, nlohmann-json, curl[ssl]; builtin-baseline pinned
build_for_windows.bat    finds VS (vswhere) + vcpkg, Ninja Release build, ctest, `--target dist`
build_for_mac.sh         same for macOS (arm64-osx / x64-osx triplets)
src/common/              std-only code, also compiled into the installer: strings, config, payload, platform, utf8_path.h, html.h/.cpp (entity/tag helpers)
src/core/                business logic, no Discord: db, repository, portfolio, analysis/, market/, news/, arena/, ml/ (Pythia AI), politicians/ (Senate trade scraper), monitor, commands, messages
src/bot/                 lib strojanka_bot: Discord adapter (D++), BotHost (lifecycle), TLS trust; main.cpp = headless exe + CLI tools
src/server/              StrojankaServer: webview window (ui/server.html embedded) hosting BotHost, dashboard, live log, console
src/installer/          setup: webview window (ui/installer.html embedded) + console fallback, shared installer_core
src/packer/main.cpp      build tool: stub + staged files -> dist/StrojankaInstaller(.exe|.app)
third_party/             vendored headers: webview 0.12.0 (MIT), WebView2.h 1.0.3485.44 (see third_party/README.md)
assets/icon/             strojanka.svg (master), .ico, .icns, png/ sizes; regenerate with tools/make_icons.ps1
cmake/embed_file.cmake   turns a file into a byte-array header (used for the installer HTML)
tests/test_main.cpp      self-contained test runner (CHECK macro), registered with ctest
documentation/           human docs (also shipped inside the installer as docs/)
agent_documentation/     this folder
brainstroming/           original task notes (archived_task.md)
```

## Build / run / test (Windows dev box)
```
build_for_windows.bat                                   # full build + tests + dist\StrojankaInstaller.exe
build\windows\bin\strojanka_tests.exe                   # tests (~2 s, offline)
build\windows\bin\strojanka.exe --analyze FCX           # live report without Discord (in-memory DB if no config)
build\windows\bin\strojanka.exe --backtest FCX KO ...   # walk-forward accuracy on 10y Yahoo data
build\windows\bin\strojanka.exe --migrate --config X    # create/upgrade DB (installer calls this)
```
Incremental build needs the MSVC env: `vcvars64.bat` then `cmake --build build\windows [--target X]`.
D++ forces C++20 on consumers; the whole project is C++20.

## Invariants (do not break)
1. **Core has no Discord types.** `src/core` must not include `<dpp/...>`. Front-ends talk to `CommandService` and implement `Notifier`.
2. **All SQL lives in `Repository`** (`src/core/repository.cpp`); every method locks `Database::mutex()` (recursive). Use `SqlTransaction` for multi-statement writes.
3. **Released migrations are immutable.** Append a new `Migration{N+1,...}` in `src/core/migrations.cpp`. Schema version = `PRAGMA user_version`.
4. **No look-ahead in analysis.** `analyze_series` state at bar i uses bars â‰¤ i only (online `ZigZag`). Backtests and live alerts share this code path.
5. **Strings are UTF-8 everywhere.** Convert paths only via `utf8_path()` / `path_utf8()` (`src/common/utf8_path.h`); never `fs::path(std::string)` for user paths (ANSI on Windows).
6. **Installer stays a single file**: static CRT, no DLLs. Allowed: `src/common/*`, std, header-only libs (nlohmann_json, `third_party/`), OS libraries. Database work happens by running `strojanka --migrate`.
7. Alert wording first sentences are fixed by the spec: "`<SYM>` has reached PEAK. You have `<N>` shares. It is time to sell." / "`<SYM>` has reached SUPPORT level. It is time to buy." (`src/core/messages.cpp`).
8. Accuracy shown to users must be **out-of-sample** (walk-forward) with the random-day baseline beside it; never in-sample calibration numbers.

9. **Version stays 0.1.1** until the user explicitly allows a further bump; new features go under a new CHANGELOG section only once that's allowed. (Schema versions are separate and do increase.)
10. **Arena bots never see the future:** `arena::step` fills orders at the *next* day's open; the test `arena engine: accounting, no look-ahead` must keep passing. Arena results reach Discord only for `public` bots.
11. **Never simulate keyboard input** (SendKeys etc.) to test the windows on the user's machine: it once typed into the user's own document.
12. **No paid services:** no Claude/LLM APIs or per-use costs; AI runs locally on a laptop CPU (see core/ml).
13. **Politician trade data is scraped, not guessed.** `politicians::SenateEfdClient` only parses electronic PTR filings from efdsearch.senate.gov; paper filings and the House (PDF-only) are out of scope. Test against `SenateSource` (fake in tests), never the real site.

## Conventions
- Namespace `strojanka`, analysis code in `strojanka::analysis`. 4-space indent, ~120 cols, `snake_case` functions, `PascalCase` types, `kConstant`.
- Errors: core throws `DatabaseError` for SQL failures; market/provider returns `std::optional` + `std::string* error`. User-facing errors are `CommandReply{false, text}`.
- Money/shares are `double` (REAL in SQLite). Shares epsilon `kShareEpsilon = 1e-9` (`portfolio.h`).
- Time: ISO-8601 UTC strings `YYYY-MM-DDTHH:MM:SSZ` (sortable); bar dates `YYYY-MM-DD` exchange-local. Helpers in `common/strings.h`.
- Logs: `log_info/warn/error/debug` (`core/log.h`), console + `log_file`.
