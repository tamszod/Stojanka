# Commands

Talk to the bot in a **direct message** (click it in the server member list, then **Message**).
Messages in server channels are ignored.

You can use the commands two ways:

- **Type them as text**, like in the original task: `buy FCX 0.858291457 59.78$`. A `/` or `!` in front is fine too.
- **Use slash commands**: type `/`, pick the command from the pop-up list, and fill in the boxes.

Both do the same thing. Below, each command is shown in its slash form.

Amounts can be typed as `59.78`, `59.78$`, `$59.78`, `59,78`, `743,04€` or `1.234,50€`.

## /help
Lists the commands and how to use them.

## /monitor stock:FCX
Starts watching a stock. The first time anyone adds a stock, the bot downloads ten years of daily
prices, learns how that stock behaves, tests itself honestly on that history, and replies with a
report like this (real output from October 2026):

```
KO - The Coca-Cola Company (NYSE)
Price: $88.05 | trend: decreasing
Category: Trending up
Volatility: 19% per year | 1-year trend: +36% | swings per year: 7.0
Support: $78.49 | Resistance: $86.14
Cycle: falling since the swing high of $91.44. Support score 39% (alert at 55%).
Learned settings: swing 3.8%; a call counts as right after a 2.9% move within 20 trading days.
Honest test (re-learned monthly on past data, calls made blind):
- PEAK calls: 11 of 26 right (42%) vs 27% for a call on a random day
- SUPPORT calls: 16 of 31 right (52%) vs 38% for a call on a random day
```

Compare the two percentages on each line. When the bot's number is not clearly higher than the
random-day number, the report says **no proven edge yet**, and alerts for that stock should be
treated as hints only.

### ETFs
ETFs work exactly like stocks: `/monitor`, `/report`, `/buy`, `/sell`, `/status`, `/profit` and the
PEAK/SUPPORT alerts all accept them. US ETFs use their usual ticker (`SPY`, `VOO`, `QQQ`). European
listings need the exchange ending Yahoo Finance uses:

| Exchange | Ending | Example |
|---|---|---|
| London | `.L` | `CSPX.L`, `VUSA.L` |
| Xetra (Germany) | `.DE` | `SXR8.DE`, `VUAA.DE` |
| Amsterdam | `.AS` | `IWDA.AS` |
| Milan | `.MI` | `CSSPX.MI` |
| Paris | `.PA` | `CW8.PA` |

If you leave the ending out (`/monitor stock:CSPX`), the bot lists the matching listings so you can
pick yours, for example `CSPX.L` (London) or `CSPX.AS` (Amsterdam). Pick the one you actually bought:
the same fund trades in different currencies on different exchanges.

Notes:
- **Prices are in the listing's currency.** Many London listings are quoted in pence; the bot converts
  them to pounds, so type amounts in pounds too.
- **News is not used for ETFs.** A fund moves with its whole basket, and headlines that mention it are
  mostly about the fund company, so they would only add noise.
- **Amounts** can be typed the European way too: `1.234,50€` is 1234.50.
- With holdings in several currencies, `/status` and `/profit` show one total per currency. They are
  never added together without conversion.

## /monitor politician:Pelosi
Personally watch a politician instead of a stock: you get a direct message the next time they
disclose a trade. As text: `monitor politician Nancy Pelosi`. See [politician trades](politicians.md).

## /remove stock:FCX
Stops watching a stock. If you still own shares of it, the bot refuses and tells you how many
you have. Record the sale with `/sell` first.

## /remove politician:Pelosi
Stops watching a politician. As text: `remove politician Nancy Pelosi`.

## /buy stock:FCX shares:0.858291457 paid:59.78$
Records a purchase. `paid` is the **total** amount you paid for those shares, not the price per
share. If you were not watching the stock yet, it is added to your watch list.

## /sell stock:FCX shares:0.858291457 received:67.87$
Records a sale. `received` is the **total** amount you got. The reply shows the profit on this
sale. You cannot sell more shares than you have recorded.

## /status
Your watch list: price, trend (increasing, decreasing or stagnating over about a month), shares
held, their current value, and profit or loss on them. The last line is your **balance**: the
current value of all your shares, what you paid for them, and the difference.

## /profit and /profit stock:FCX
Your profit for every stock you have traded, or for one stock.

- **Realized:** money received from sales minus what those shares cost you (selling price - buying price).
- **Unrealized:** what the shares you still hold are worth now minus what you paid for them.

If you bought at several prices, the bot uses your **average cost**. Example: you buy 2 shares for
$100, then 2 more for $140. Your average cost is $60 per share. Selling 1 share for $75 is a
realized profit of $15.

## /report stock:FCX
A fresh analysis report for any stock, the same as the one `/monitor` shows. You don't need to be
watching the stock.

## /news stock:FCX
The latest headlines about a stock, each marked by its tone: 🟢 positive, 🔴 negative, ⚪ neutral,
⛔ red flag. Below them is the three-day summary and how much news currently influences the
signals. As text: `news FCX`.

```
🟢 2026-10-12 JPMorgan raises target for Freeport-McMoRan stock to USD 92.00
🔴 2026-10-12 Intel Stocks Fall 3.6% as Cheap Chinese Models Spread Faster (Yahoo Finance)
News: slightly negative (26 headlines in 3 days).
```

Reports (`/monitor`, `/report`) and alerts include the same one-line news summary.

## /suggest, /suggest side:buy, /suggest side:sell
Lists the stocks whose PEAK or SUPPORT calls have actually worked, so you can pick stocks the
bot is good at. As text: `suggest`, `suggest buy` or `suggest sell`.

```
STOCK  GOOD AT  RECORD      RANDOM  NOW
BMY    PEAK     12/25 48%   30%     falling
JPM    SUPPORT  14/25 56%   38%     falling, NEAR SUPPORT
```

- **GOOD AT:** which call the stock has a good record for. PEAK means sell signals, SUPPORT means buy signals.
- **RECORD:** how many of those calls were right in the honest test (ten years, settings re-learned monthly, calls made without seeing the future).
- **RANDOM:** how often the same call would have been right on a random day.
- **NOW:** the current swing. *NEAR SUPPORT* or *NEAR PEAK* means an alert for that stock may come soon.

A stock is listed only if it beats RANDOM even when judged skeptically, with 90% confidence.
The bot checks about 70 large US stocks once a day in the background. The very first `/suggest`
after installing may ask you to wait a minute or two. To check different stocks, set
`screener_symbols` in the settings file.

**Be careful with this list.** When you test many stocks, a few will look good just by luck. In
October 2026 only 2 of 70 stocks passed, which is about what luck alone would produce. Treat a
suggestion as a candidate: add it with `/monitor` and watch whether its live alerts keep being
right.

## /arena and /arena bot:Diana
The trading-bot arena: bots that each trade $1,000 of play money with a different strategy, named
after Greek and Roman gods. `/arena` shows the leaderboard of the bots that have proven
themselves, and Atlas, who simply holds every stock. `/arena bot:Diana` shows one bot: its
strategy, its test on old prices, its live results, what it holds and its latest trades. As text:
`arena` or `arena diana`. Bots still in training or proving themselves stay private. See
[the arena guide](arena.md).

## /pythia
The built-in AI model: how well it did on years it never saw, and today's stocks with the
highest chance of a rise or a fall. As text: `pythia` or `ai`. See [Pythia](pythia.md).

## /politicians and /politicians name:Pelosi
Recent US Senate stock trade disclosures: everyone's, or just one politician's. As text:
`politicians` or `politicians Pelosi`. See [politician trades](politicians.md).

## /admin (admins only)

Admins look after the bot. **Whoever created the bot in the Discord developer portal is
automatically an admin.** They can add others, and every admin receives the bot's warnings.

| Command | What it does |
|---|---|
| `/admin status` or `admin` | The admin panel: database location and size, free disk space, number of users, stocks, trades, alerts and stored headlines, the last price check, the news weight, and the admin list. |
| `/admin add user:@someone` or `admin add <user ID>` | Makes someone an admin. To copy an ID, turn on Developer Mode in Discord, right-click the person and choose **Copy User ID**. |
| `/admin remove user:@someone` or `admin remove <user ID>` | Removes an admin. The bot's owner can't be removed. |
| `admin channel <channel ID>` | Sets the Discord channel where public arena bots and politician trade disclosures are posted. Without an ID, it shows the current channel. |
| `/admin movedb folder:D:\StrojankaData` or `admin movedb D:\StrojankaData` | Moves the database to another folder or drive **while the bot keeps running**. The bot copies the database, checks the copy, switches to it, deletes the old file and updates the settings file. If anything goes wrong, nothing changes. |

**Warnings admins receive** (as direct messages):

- **Low disk space:** sent when the drive holding the database has less free space than
  `low_disk_warning_mb` in the settings (default 1024 MB). It's checked every hour and repeated at
  most once a day. The message suggests `admin movedb`.
- **Price data unavailable:** sent when two price checks in a row could not refresh a single
  stock, for example because the internet connection is down. It's repeated at most every 6 hours.

## Alerts
Alerts arrive as direct messages:

- **PEAK**: *FCX has reached PEAK. You have 0.858291457 shares. It is time to sell.* You only get this if you own shares.
- **SUPPORT**: *FCX has reached SUPPORT level. It is time to buy.* Everyone watching the stock gets this.

You get at most one alert per swing.
