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
- **Watched stocks** (Ctrl+1): every stock any user watches, with its company name under the
  ticker (hover for the full name), price, trend, number of watchers and the last alert.
- **Markets** (Ctrl+3): stock exchanges, their hours and whether they're open. See [Markets](#markets).
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
| `stocks` | Watched stocks with company, price, trend, watchers and last alert |
| `users` | Discord users: how many stocks each watches and holds, and who is an admin |
| `check` | Runs a price check for all watched stocks now |
| `screen` | Re-runs the `suggest` screener now (about 15–30 seconds) |
| `report FCX`, `news FCX`, `suggest [buy\|sell]`, `pythia` | The same as in Discord |
| `as <user ID> <command>` | Runs a Discord command as that user, e.g. `as 355014703254339595 status` to see their portfolio |
| `arena` | All trading bots, including the ones still in training or proving themselves |
| `arena Diana` | One bot in detail |
| `arena run` | Trade and re-judge the bots right now, instead of waiting for the hourly pass |
| `arena channel <channel ID>` | The Discord channel where public bots report |
| `backup` | Save the database as a `.strojanka_backup` file now (same as the **Backup** button) |
| `market`, `market London` | Stock exchanges: ending, hours, open or closed (same as in Discord) |
| `politicians`, `politicians Pelosi` | Recent US Congress stock trades, everyone's or one politician's |
| `politicians run` | Check for new House and Senate filings right now, instead of waiting for the daily pass |
| `politicians channel <channel ID>` | Same shared public channel as `arena channel` |
| `admin add <user ID>`, `admin remove <user ID>` | Manage admins |
| `admin movedb <folder>` | Move the database to another folder or drive while running |
| `loglevel debug\|info\|warn\|error` | How much the console shows (until the next restart) |
| `version`, `clear` | Version number; empty the console |

## Backups

Press **Backup** in the top bar (or type `backup`) to save the whole database as one file,
`strojanka-<date>.strojanka_backup`, in the `backups` folder of the install folder. Set
`backup_folder` in the settings to save them elsewhere, ideally on another drive or in a cloud
folder. The **Database** card shows when the last backup was made.

- **Reminders:** after 3 weeks without a backup, admins get a Discord message, and an email if an
  address is set; then again every 2 days until you make one. The setup asks for the address; later
  use `admin email you@example.com` and put the mailbox's password in `smtp_password` in the
  settings (for Gmail an [app password](https://myaccount.google.com/apppasswords), because your normal
  password won't work). `admin email test` or `strojanka --test-email` checks that mail arrives. Gmail,
  Outlook/Hotmail, Yahoo and iCloud are set up automatically; for other mail set `smtp_server`, for
  example `smtps://smtp.example.com:465`.
- **Restoring:** run the setup, choose **Change settings** (or a new install), and under
  **Database** pick **Restore a backup**. A database already in that place is kept beside it.

## Markets

The **Markets** tab (Ctrl+3) lists every exchange with its ticker ending, its hours in this
computer's time and whether it is open; markets with watched stocks are marked. Prices of a stock
are only checked while its market is open (plus once just after the close).

## Running without a window

On a server, or to start the bot from a script, use `start_strojanka.bat` (Windows) or
`start_strojanka.command` (Mac). It runs the same bot in a plain console window.

Only one copy can run at a time. If the window and a console are started together, the second
one refuses to start and says so. Otherwise every alert would be sent twice.

## If Strojanka closes by itself

Open `strojanka.log` (the **Log** button, or the install folder) and look at the last lines before
the gap and the first lines of the next start:

- **A reason before the gap**, such as *Windows is shutting down or restarting: stopping
  Strojanka* or *an installer or Windows Update asked running programs to close*: it didn't crash,
  Windows ended it. Windows Update restarts at night unless you set **active hours** or pause
  updates. The bot doesn't start again by itself after a restart.
- **`the computer is going to sleep`**: nothing runs while the computer sleeps. Change
  **Settings > System > Power** if the bot should keep running.
- **`CRASH:` lines**, or *the previous run … did not shut down cleanly* at the next start: it
  crashed. Please open an issue and attach `strojanka.log`, plus `strojanka-crash.log` and
  `strojanka-crash.dmp` from the install folder if they exist.

To have it start again by itself after Windows restarts, run the installer and tick **Start with
Windows** (also offered when updating). It then opens minimized each time you sign in, and waits
up to three minutes for the internet connection before connecting to Discord.

Windows keeps its own record too. This PowerShell command lists restarts, shutdowns and
Strojanka crashes from the last week (it only reads):

```powershell
$since = (Get-Date).AddDays(-7)
Get-WinEvent -FilterHashtable @{LogName='System'; Id=41,1074,6005,6006,6008; StartTime=$since} -ErrorAction SilentlyContinue | Format-Table TimeCreated, Id, Message -Wrap
Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000,1001,1002; StartTime=$since} -ErrorAction SilentlyContinue | Where-Object Message -match 'strojanka' | Format-Table TimeCreated, Id, Message -Wrap
```

ID 1074 is a restart or shutdown and who asked for it (`TrustedInstaller.exe` = Windows Update),
41 or 6008 an unexpected power loss, and 1000 to 1002 a crash or hang of Strojanka.
