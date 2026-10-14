# The Strojanka Server window

**Strojanka Server** is the normal way to run the bot. Open it from the Start menu (or
`StrojankaServer.exe` / `StrojankaServer.app` in the install folder). The bot connects to Discord
as soon as the window opens, and stops when you close it.

## What you see

- **Top bar:** whether the bot is online and under which Discord name, how long it has been
  running, and buttons to open the settings file, the install folder and the log file.
  **Stop** / **Start** disconnects or reconnects the bot without closing the window.
- **If something is wrong** (for example, Discord rejected the token), a red banner explains it,
  with **Edit settings** and **Try again** buttons.
- **Cards:** users, watched stocks, alerts sent, stored news headlines and the current news
  weight, database size and location, free disk space (the bar turns amber, then red, as space
  runs low), and when prices were last checked.
- **Watched stocks** (Ctrl+1): every stock any user watches, with price, trend, number of watchers
  and the last alert.
- **Arena** (Ctrl+2): every trading bot with its stage (training, proving, public), its $1,000
  portfolio's value, its return and its lead or lag compared with Atlas, its trades, and its test
  on old prices. Click a bot for its details. See [the arena guide](arena.md).
- **Console:** everything the bot does, live. Warnings are yellow and errors red. **Warnings**
  and **Errors** hide the ordinary lines.

## The command line

Type at the bottom of the console and press Enter. ↑ and ↓ go through earlier commands, and Tab
completes a command name. Whoever sits at the server is trusted, so admin commands work here
without being a Discord admin.

| Command | What it does |
|---|---|
| `help` | Lists the commands |
| `status` | The admin panel: database, disk space, users, last price check, admins |
| `stocks` | Watched stocks with price, trend, watchers and last alert |
| `users` | Discord users: how many stocks each watches and holds, and who is an admin |
| `check` | Runs a price check for all watched stocks now |
| `screen` | Re-runs the `suggest` screener now (about 15–30 seconds) |
| `report FCX`, `news FCX`, `suggest [buy\|sell]`, `pythia` | The same as in Discord |
| `as <user ID> <command>` | Runs a Discord command as that user, e.g. `as 355014703254339595 status` to see their portfolio |
| `arena` | All trading bots, including the ones still in training or proving themselves |
| `arena Diana` | One bot in detail |
| `arena run` | Trade and re-judge the bots right now, instead of waiting for the hourly pass |
| `arena channel <channel ID>` | The Discord channel where public bots report |
| `politicians`, `politicians Pelosi` | Recent US Senate stock trades, everyone's or one politician's |
| `politicians run` | Check for new Senate filings right now, instead of waiting for the daily pass |
| `politicians channel <channel ID>` | Same shared public channel as `arena channel` |
| `admin add <user ID>`, `admin remove <user ID>` | Manage admins |
| `admin movedb <folder>` | Move the database to another folder or drive while running |
| `loglevel debug\|info\|warn\|error` | How much the console shows (until the next restart) |
| `version`, `clear` | Version number; empty the console |

## Running without a window

On a server, or to start the bot from a script, use `start_strojanka.bat` (Windows) or
`start_strojanka.command` (Mac). It runs the same bot in a plain console window.

Only one copy can run at a time. If the window and a console are started together, the second
one refuses to start and says so. Otherwise every alert would be sent twice.
