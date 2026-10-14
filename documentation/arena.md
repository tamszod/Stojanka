# The arena: trading bots

The arena is a competition between trading bots. Each bot follows one strategy and starts with
**$1,000 of play money**. It buys and sells real stocks at real prices, but no real money is
involved. The arena shows which strategies actually work, and the proven bots report their
trades and profits in your Discord server.

## The bots

Each bot is named after a Greek or Roman god or oracle whose character fits its strategy. Most come in
pairs: the Greek god and the Roman twin try the same idea with different settings.

| Bot | Named after | Strategy |
|---|---|---|
| **Atlas** | the Titan who carries the world | Buys every stock on day one and holds them all. Atlas *is* the market, the benchmark every other bot must beat. |
| **Athena** | Greek goddess of wisdom | Buys at Strojanka's own SUPPORT signals and sells at its PEAK signals (8% stop-loss, at most 40 days per trade). |
| **Minerva** | Roman goddess of wisdom | Like Athena, but only acts on the strongest signals. |
| **Ares** | Greek god of war | Follows trends: holds a stock while its 20-day average price is above its 100-day average. |
| **Mars** | Roman god of war | Follows long trends: 50-day average above the 200-day average. |
| **Hermes** | the fastest god, the messenger | Momentum: each month, holds the 5 stocks that rose most over 6 months. |
| **Mercurius** | Roman messenger god | Momentum over 12 months, ignoring the most recent month. |
| **Artemis** | Greek goddess of the hunt | Buys a stock that closes above its highest price of the last 20 days, sells below its 10-day low. |
| **Diana** | Roman goddess of the hunt | The same with 55 and 20 days: fewer, longer hunts. |
| **Nemesis** | goddess of retribution for excess | Buys sharp 2-day drops in stocks that are in a long rise, sells on the rebound. |
| **Iustitia** | Roman goddess of justice | Buys only extreme drops and waits for a full recovery. |
| **Pythia** | the oracle of Delphi | Trades on the built-in AI model ([Pythia](pythia.md)): buys stocks it gives at least a 50% chance of a clear rise, for up to 20 days. |
| **Sibylla** | the Roman oracle, the Sibyl | The same, but only at a 55% chance or more. |

The bots trade the same list of about 70 large US stocks that `/suggest` checks (or
`screener_symbols` from the settings). A bot holds at most 5 stocks at a time.

## How a bot earns the right to report in public

Every bot goes through three stages:

1. **Training: the test on old prices.** The arena replays about 8 years of history and lets the
   bot trade it with $1,000. It passes if:
   - it made money in each third of the period, so a single lucky year isn't enough,
   - its worst fall from a high stayed under 35%,
   - its returns were large compared with its ups and downs (Sharpe ratio of at least 0.6),
   - it completed at least 10 trades, enough to judge.
2. **Proving: live trading in private.** It gets a fresh $1,000 and trades with real, new prices
   every day. Only the server window shows it. After **40 trading days** (about two months), it
   goes public if it kept up with Atlas over that time (at most 2 percentage points behind) and
   never fell more than 15%. Otherwise it keeps proving itself, judged on its most recent 40 days.
3. **Public.** Its trades are announced in the Discord channel, for example:

   > 🏛️ **Diana** traded on 2026-12-03:
   > - bought **CAT**: 0.4130 shares at $484.20 ($200.00)
   > - sold **XOM**: 1.6400 shares at $121.95 ($200.00), profit **+$14.80**
   >
   > Portfolio: **$1,084.12** (+8.4% since 2026-10-12)

   Every week the channel also gets a leaderboard.

**Bots are watched all the time.** Every week the arena re-runs the history test with the
newest prices. A public bot goes back to proving itself if it fails that test, falls more than
25% from its high, or ends up 10 percentage points behind Atlas over 60 trading days. The
channel is told when this happens.

## Honest trading rules

- A bot decides after the day's closing prices are known, and its orders are filled at the
  **next day's opening price**. It can never act on prices it could not have seen.
- Every fill costs 0.1%, for the gap between buying and selling prices and for fees.
- A trading day counts once the US market has closed (from 21:30 UTC).

## Read the history test with care

The history test uses **today's** list of large stocks. Companies that grew huge (and joined the
list) are in it; companies that shrank and dropped off are not. That flatters every strategy,
especially the ones that buy strong stocks. For example, Hermes turned $1,000 into about $21,000
in the test, and even Atlas turned it into about $5,000. Compare each bot with Atlas, who has the
same advantage, and trust the live results more. That is exactly why no bot reports in public
before proving itself live.

## Setting it up

The arena runs by itself inside the bot. It starts as soon as the bot connects to Discord and
then works once an hour. The first pass downloads about 10 years of prices for each stock, which
takes a few seconds. The only thing to set is the Discord channel for public reports:

1. In Discord, turn on Developer Mode (Settings → Advanced), right-click the channel and choose
   **Copy Channel ID**.
2. Send `admin channel <channel ID>` to the bot, or type `arena channel <channel ID>` in the
   server window's console.
3. Make sure the bot can send messages in that channel.

## Following the arena

- In Discord: `/arena` shows the leaderboard of public bots and Atlas, and `/arena bot:Diana`
  shows one bot in detail. Bots that haven't proven themselves yet stay private.
- In the server window: the **Arena** tab (Ctrl+2) lists every bot, including those in training
  and proving. Click a bot for its details. The console also accepts `arena`, `arena Diana` and
  `arena run`, which runs a pass right now.

The arena is a learning tool. Play money only; nothing here is investment advice.
