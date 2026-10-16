# Strojanka

Strojanka is a Discord bot that watches stocks for you. It tells you when a stock you own has
probably reached the **top of its current swing (PEAK)**, so it may be time to sell, and when a
stock you watch has fallen to a **floor (SUPPORT)**, so it may be time to buy.

It also keeps a simple record of what you bought and sold, so it can show you your balance and
your profit.

> Strojanka's signals are statistical estimates based on past prices. They are not financial
> advice, and no tool can predict the market reliably.

## What it does

- **Watches stocks for many people at once.** Each Discord user has their own watch list and their own trades.
- **Works with ETFs too**, US ones (`SPY`) and European listings (`CSPX.L`, `SXR8.DE`), in their own currency.
- **Checks prices regularly** (every 15 minutes by default), all stocks in parallel.
- **Writes a report when you add a stock:** what kind of stock it is, where its support and resistance are, and how accurate the bot's calls would have been in the past.
- **Learns each stock's behaviour.** It tunes its settings separately for every stock, re-checks them every day, and grades every alert it sent. If its alerts for a stock keep being wrong, it becomes stricter for that stock.
- **Sends alerts as direct messages:**
  - *FCX has reached PEAK. You have 0.858291457 shares. It is time to sell.* (only sent if you own the stock)
  - *FCX has reached SUPPORT level. It is time to buy.*

- **Has a small built-in AI, Pythia**, that runs on your processor and is tested honestly on
  years it never saw.
- **Runs a trading-bot arena.** Thirteen bots named after Greek and Roman gods trade $1,000 of play
  money each. Only the ones that prove themselves on years of old prices *and* months of live
  prices report their trades and profits in your Discord server.
- **Tracks politician stock trades.** Checks daily for newly disclosed US Senate stock trades and
  posts them in your Discord server; you can also watch a specific politician.

## Guides

| Guide | For |
|---|---|
| [Installing](installing.md) | Setting up the Discord bot and running the installer |
| [The server window](server.md) | Running the bot: dashboard, live log and the server command line |
| [Commands](commands.md) | Everything you can type in Discord |
| [Pythia, the built-in AI](pythia.md) | What the AI model predicts, how it is tested, and how well it does |
| [The arena](arena.md) | Trading bots with $1,000 of play money, and how they prove themselves |
| [Politician trades](politicians.md) | Tracking US Senate stock trade disclosures |
| [How the signals work](how-signals-work.md) | PEAK, SUPPORT, stock categories and how the bot learns |
| [Building from source](building.md) | Developers: building, testing, versioning |

## Quick start

1. Create a Discord bot and copy its token ([details](installing.md#1-create-the-discord-bot)).
2. Run `StrojankaInstaller.exe` (Windows) or `StrojankaInstaller.app` (Mac) and follow the setup window.
3. Start **Strojanka Server** from the Start menu. Its window shows the bot's status and log, and has a command line.
4. In Discord, open a direct message with the bot and type `help`.

Current version: see the `VERSION` file and [CHANGELOG.md](../CHANGELOG.md).
