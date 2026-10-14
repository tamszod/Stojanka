# How the signals work

## Swings, peaks and supports

Most stock prices move in swings: up for a while, down for a while. Strojanka follows these swings.

- A **swing low** is confirmed once the price has risen a certain percentage above it. A **swing
  high** is confirmed once the price has fallen that percentage below it. That percentage, the
  *swing size*, is different for every stock. A calm utility stock might use 4%, a mining stock
  12%.
- After a confirmed swing low the stock is in a **rising leg**, and the bot watches for the
  **PEAK**. After a confirmed swing high it is in a **falling leg**, and the bot watches for
  **SUPPORT**.
- **Resistance** is the average of the last few swing highs and **support** is the average of the
  last few swing lows. These are the price levels where the stock has turned before.

## The score

On every check the bot scores the current leg from 0% to 100%. For a PEAK, the score combines:

| Ingredient | Meaning | Weight |
|---|---|---|
| Swing size | How far the price has risen compared with this stock's typical rise | 30% |
| RSI | A standard measure of how *overbought* the stock is | 25% |
| Resistance | How close the price is to earlier swing highs | 20% |
| Turning | Whether the price has started to slip from its high | 15% |
| Bollinger band | Whether the price is stretched far above its recent average | 10% |

SUPPORT works the same way in the other direction. When the score reaches the stock's alert
level, the alert is sent, once per swing.

## How the bot learns

### What counts as a right call
An alert counts as **right** if the price then moved in the predicted direction by a set amount
(based on the stock's volatility, for example 6%) **before** moving half that amount the other way,
within 20 trading days. If the price went the wrong way first, the alert was **wrong**. If nothing
happened in 20 days, it counts as **expired**, which also counts against it.

### When a stock is added
The bot replays the last two years of the stock's prices. It tries 120 combinations of settings
(swing size, RSI levels, alert level) and keeps the one whose alerts would have been right most
often. Recent history counts more than old history: a call from a year ago weighs half as much as
one from today. So when a stock changes character, the settings follow it.

### The honest test
Settings chosen on the same data they are scored on always look better than they really are. So
the bot also runs a fair test over ten years of history. It pretends to live through those years:
every month it re-learns its settings using only the past, then makes calls on the following month
without seeing it, and grades them.

As a yardstick it also grades a call made on *every* day. That is the score of a caller with no
skill at all. Because stocks tend to rise over time, a random "buy" call is right more often
(about 38%) than a random "sell" call (about 28%).

The report and every alert show the honest score next to the yardstick. If the bot is not at least
5 points better over at least 8 calls, it says **no proven edge yet**.

### Every day
The settings are re-learned on the newest data.

### After every alert
The bot keeps every alert it sent. Once enough days have passed, it grades each one right, wrong
or expired. If fewer than half of a stock's last 10 graded alerts were right, the bot raises that
stock's alert level, so it only speaks up when it is more certain. If 75% or more were right, it
slowly lowers the level again. This is how it learns from its mistakes.

## Stock categories

Every stock is placed in a category based on its last year of prices:

| Category | How it is recognised | What it means for the signals |
|---|---|---|
| **Cyclical / range-bound** | At least 4 swings per year, no clear trend | What PEAK and SUPPORT calls are designed for. |
| **Trending up** | Rising more than 20% a year in a fairly straight line | Prices that look "too high" often keep rising. Be careful with PEAK calls. |
| **Trending down** | Falling more than 20% a year in a fairly straight line | Prices that look "cheap" often keep falling. Be careful with SUPPORT calls. |
| **Speculative / high volatility** | Yearly volatility above 60% | Levels break often. Treat calls with caution. |
| **Defensive / low volatility** | Yearly volatility below 18% | Small swings, fewer calls, smaller profit per swing. |
| **Mixed** | None of the above | No dominant behaviour yet. |

These categories describe how a stock behaves, not which industry it is in. Industry sectors
(mining, tech and so on) could be added later as a second label. In this version the category is
shown for information. It does not change how the signals are calculated.

## How accurate is it, really?

In this first version: **on most stocks, not better than chance yet.** Measured with the honest test
on 36 well-known US stocks over ten years:

| | Bot right | Random day right |
|---|---|---|
| PEAK calls | 25-27% | 28-29% |
| SUPPORT calls | 33-41% | 37-38% |

Some stocks do better. Coca-Cola (KO) beats the yardstick on both sides. On a perfectly cyclical
test price the bot is right 19 times out of 20, so the machinery works, but real prices are much
less regular. Selling at "overbought" peaks in rising stocks is especially unreliable, because
strong stocks often keep rising.

That is why every report and alert shows its track record. Improving the model is the main goal of
the next versions, and the honest test is how improvements will be proven.

## News

About once an hour the bot reads the latest headlines for each watched stock from Yahoo Finance
and Google News. It drops repeats and stories that don't name the company, and judges each
headline's tone with a list of finance words and phrases: "beats estimates" and "raises price
target" count as positive, "misses estimates", "cuts guidance" and "downgrade" as negative, and
negations like "fails to beat" are handled. A headline phrased as a question ("Is X a buy?") counts
for half.

News is used in three ways:

- **It nudges the scores.** The news tone from the last three days, with recent headlines
  weighing more, shifts the PEAK and SUPPORT scores a little. Good news makes a buy call slightly
  more likely and a sell call slightly less likely; bad news does the opposite. It starts at a
  small weight, at most 5 points on the 100-point scale.
- **Red flags block buy alerts.** Headlines about bankruptcy, fraud, a regulator probe or
  delisting stop SUPPORT alerts for that stock, however good the chart looks.
- **It learns whether news helps.** Every alert records the news tone at that moment. Once at
  least 10 graded alerts where news agreed with the call and 10 where it disagreed exist, the bot
  compares them. If agreeing news clearly went with better calls, news gets more weight, up to 15
  points. If not, it gets less, down to zero.

**Why only a small, learned weight?** Free news feeds go back only a few weeks, so news cannot be
put through the ten-year honest test that prices go through. The bot stores every headline it
reads, permanently: hourly for watched stocks, and daily for the ~70 stocks the `/suggest`
screener checks. So the history needed to test news properly grows from now on, and later
versions can use it to measure exactly how much news improves the calls. Reading tone from
headlines with a word list is also rough: it gets "cuts price target" right, but not sarcasm or
subtle wording.

## The AI model

Besides the chart signals, Strojanka has a small built-in AI, **Pythia**. It learns from about 25
measurements across 70 stocks and 10 years, and gives each stock a chance of a rise or a fall
within 20 days. It runs on your processor, with no internet AI service. Tested on years it never
saw, it has not beaten an average day so far, so it is shown as extra information and is not
used to send alerts. See [Pythia](pythia.md).

## Finding predictable stocks

Because the model only works on some stocks, `/suggest` runs the honest test on a list of about
70 large US stocks every day and lists the ones where it clearly beats the random-day yardstick.
"Clearly" means the bot judges its own record skeptically: it uses the lowest hit rate that is
still 90% likely given the record, not the hit rate itself. A record of 5 right out of 8 does
not count, but 15 out of 25 might.

Even so, testing 70 stocks on two kinds of call makes 140 tests, and a handful will pass by luck.
So a suggestion is a candidate, not a promise. Watching its live alerts with `/monitor` is how
you find out whether it was skill or luck.

## Limits to keep in mind

- Prices come from Yahoo Finance's public data and can be delayed by about 15 minutes.
- The analysis uses daily prices. It finds swings that last days to weeks, not minutes.
- A strong trend can carry a stock far past its old resistance. No indicator catches every top.
- Past accuracy does not guarantee future accuracy.
