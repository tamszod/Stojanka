# Building from source

## Windows

Requirements: Visual Studio 2022 or newer with the **Desktop development with C++** workload
(it includes CMake, Ninja and vcpkg), and Git.

```
build_for_windows.bat
```

The script finds Visual Studio, finds or downloads vcpkg, builds everything in Release mode, runs
the tests, and writes **`dist\StrojankaInstaller.exe`**. The first build takes a while (around 15
to 30 minutes) because the libraries are compiled from source. Later builds reuse them.

## macOS

Requirements: Xcode command line tools (`xcode-select --install`), Git and Homebrew packages:

```
brew install cmake ninja pkg-config
./build_for_mac.sh
```

This writes **`dist/StrojankaInstaller.app`** for the Mac's own architecture (Apple Silicon or Intel).

## What gets built

| Target | What it is |
|---|---|
| `StrojankaServer` | The bot in a window: dashboard, live log, command line (`src/server`, interface in `src/server/ui/server.html`) |
| `strojanka` | The same bot without a window, plus command-line tools (`--analyze`, `--suggest`, `--move-db`…) |
| `strojanka_bot` | Library both of them use: Discord connection and `BotHost` (starting and stopping the bot) |
| `StrojankaInstaller` | The installer (no libraries needed, runs on a clean machine) |
| `strojanka_pack` | Build tool that appends the program files to the installer |
| `strojanka_tests` | Unit tests |
| `dist` | Puts the program files in `build/.../stage` and packs them into `dist/` |

Libraries installed by vcpkg (see `vcpkg.json`): **D++** (Discord), **SQLite**, **libcurl**
(price downloads) and **nlohmann/json**. The installer window uses **webview** and Microsoft's
**WebView2** header, which are copied into `third_party/` so the installer stays a single file.

## The installer window

The setup window is a web page, `src/installer/ui/installer.html` (HTML, CSS and a little
JavaScript), shown with the operating system's own browser engine. It is compiled into the
installer, so to change the look, edit that file and rebuild. The actual installing is done in
C++ (`src/installer/installer_core.cpp`), and the window and the text mode share that code.

## The icon

The icon is designed in `assets/icon/strojanka.svg`. The files Windows and macOS need
(`strojanka.ico`, `strojanka.icns` and PNGs from 16 to 1024 pixels) are generated from the same
shapes by:

```
powershell -ExecutionPolicy Bypass -File tools\make_icons.ps1
```

The generated files are committed, so you only run this after changing the design. The script
draws the shapes itself, so if you change the SVG, make the same change in the script. The
512-pixel PNG is a good Discord bot avatar (developer portal → your app → **Bot** → **Icon**).

## Useful commands while developing

```
strojanka --analyze FCX          # analysis report in the console, no Discord needed
strojanka --backtest FCX KO AAPL # honest accuracy test on 10 years of data (see how-signals-work.md)
strojanka --arena                # run the trading-bot arena once and print every bot (see arena.md)
strojanka --migrate --config X   # create or upgrade a database
ctest --test-dir build\windows --output-on-failure
```

## Versioning

The version number lives in one place, the `VERSION` file. CMake reads it into the program, the
installer and the settings file. Strojanka uses MAJOR.MINOR.PATCH:

- **PATCH** (0.1.0 → 0.1.1) for bug fixes,
- **MINOR** (0.1.0 → 0.2.0) for new features,
- **MAJOR** for changes that break compatibility.

To release: update `VERSION`, add a section to `CHANGELOG.md`, run the build script, and
distribute the files in `dist/`.

## Changing the database

The database layout is versioned separately (the *schema version*). To change it, append a new
migration to the list in `src/core/migrations.cpp` with the next number. **Never edit a migration
that has already been released**, because existing databases have already run it. On start-up,
the bot and the installer back up the database and apply any missing migrations in order.
