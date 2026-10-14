# Installing Strojanka

## 1. Create the Discord bot

1. Open <https://discord.com/developers/applications> and click **New Application**. Give it a name, for example *Strojanka*.
2. Open the **Bot** tab and click **Reset Token**. Copy the token and keep it secret: anyone with it can control your bot.
3. Open **OAuth2 → URL Generator**. Tick the scopes **bot** and **applications.commands**. Under bot permissions tick **Send Messages**.
4. Open the generated link and add the bot to your server.

You talk to Strojanka in a **direct message** with the bot, not in a server channel. The bot only
needs to be in one server with you so Discord lets you message it: click its name in the
member list and choose **Message**. It ignores messages posted in servers. It needs no privileged
intents.

## 2. Run the installer

Run `StrojankaInstaller.exe` on Windows or `StrojankaInstaller.app` on a Mac. A setup window opens
and guides you through four steps:

1. **Welcome:** choose the install folder. The default is `%LOCALAPPDATA%\Programs\Strojanka` on
   Windows and `~/Applications/Strojanka` on a Mac. No administrator rights are needed.
2. **Settings:**
   - Paste the bot token from step 1. The window warns you if what you pasted doesn't look like a token, for example the Public Key.
   - Choose a new database, or an existing `strojanka.db`, for example one from another computer.
   - Pick how often prices are checked.
   - On Windows, choose whether to add Start menu and desktop shortcuts.
3. **Install:** a progress bar shows each step.
4. **Finish:** tick **Start Strojanka now** to launch the bot straight away.

On Windows the installer also adds Strojanka to **Settings → Apps → Installed apps**, where you
can uninstall it later.

The window uses the Microsoft Edge WebView2 component that comes with Windows 10 and 11. On the
rare computer without it, the installer automatically switches to a text version that asks the
same questions.

**On a Mac**, macOS may block an app downloaded from the internet. If it does, right-click the
installer, choose **Open**, or run `xattr -dr com.apple.quarantine StrojankaInstaller.app` in Terminal.

## Update, repair or uninstall

Run the installer again and choose the same folder. It recognises the existing installation and
offers:

| Option | What it does |
|---|---|
| **Update** | Shown when the installer is newer. It installs the new version and keeps your token, database and settings. |
| **Repair** | Shown when the versions are the same. It checks every program file and restores only the ones that are missing or damaged. Your data is untouched. |
| **Change settings** | Reinstalls with a new token, database or check interval. |
| **Uninstall** | Removes the program, its shortcuts and its app-list entry. Your database and settings are kept unless you tick **Also delete my data**. Data stored outside the install folder is never deleted. |

Close the bot's window before updating, repairing or uninstalling. If it is still running, the
installer tells you instead of failing halfway.

### Without the window (scripts and servers)

```
StrojankaInstaller --yes --dir "D:\Strojanka" --token "YOUR_TOKEN"          # install
StrojankaInstaller --yes --dir "D:\Strojanka"                                # update or repair, keeps settings
StrojankaInstaller --yes --dir "D:\Strojanka" --uninstall [--remove-data]    # uninstall
StrojankaInstaller --console                                                 # text installer with questions
```

Run `StrojankaInstaller --help` for all options. On Windows, run these from `cmd` with
`start /wait StrojankaInstaller ...` so the prompt waits for the installer to finish.

## 3. Start the bot

Use the **Strojanka** Start menu shortcut. It opens the **Strojanka Server** window, with the
bot's status, a dashboard, the live log and a command line ([details](server.md)). Keep it open;
closing it stops the bot. Logs are also written to `strojanka.log`.

For the trading-bot arena to post in your server, choose a channel once: send
`admin channel <channel ID>` to the bot (see [the arena guide](arena.md#setting-it-up)).

To run the bot without a window, for example on a server, use `start_strojanka.bat` (Windows) or
`start_strojanka.command` (Mac) in the install folder.

In Discord, open a direct message with the bot and type `help`.

## Database upgrades

When a new version changes the database layout, the database is upgraded automatically during
an update. **Before upgrading, a backup copy is saved next to it**, named like
`strojanka.db.backup-v1-2026-10-10T12-00-00Z`.

## Putting the database on another drive

The database can live on any drive, for example a bigger data drive:

- **When installing:** under **Database**, click **Browse…** and pick a folder on that drive.
- **Later, while the bot runs:** an admin sends `admin movedb D:\StrojankaData` to the bot in
  Discord. It copies the database there, checks the copy, switches over, deletes the old file
  and updates `strojanka.conf`. Nothing needs restarting.
- **Later, with the bot stopped:** run `strojanka --move-db D:\StrojankaData` in the install folder.

The bot checks free space on the database's drive every hour. When it drops below
`low_disk_warning_mb` (1 GB by default), every admin gets a direct message suggesting a move.
The database grows slowly, mostly from price history and stored news headlines: expect a few
megabytes per watched stock per year.

## Moving to another computer

Everything the bot knows is in a single file, `strojanka.db`. Its location is in `strojanka.conf`
(by default `data\strojanka.db` inside the install folder). To move:

1. Stop the bot.
2. Copy `strojanka.db` to the new computer.
3. Run the installer there and choose **Use an existing database file**.

If the copied database is from an older version, it is upgraded (with a backup). A database from
a *newer* version is refused, so you never lose data by running old software on it.

## The settings file

`strojanka.conf` is a plain text file in the install folder:

```
discord_token=...
database_path=C:\Users\me\AppData\Local\Programs\Strojanka\data\strojanka.db
poll_interval_minutes=15
max_parallel_fetches=4
log_file=strojanka.log
log_level=info
installed_version=0.1.0
screener_symbols=
low_disk_warning_mb=1024
```

`screener_symbols` lists the stocks `/suggest` checks, separated by commas (for example
`KO,PEP,JPM,XOM`). Leave it empty to use the built-in list of about 70 large US stocks.

Restart the bot after editing it. `log_level=debug` shows the parameters learned for each stock.
