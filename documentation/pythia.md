# Pythia, the built-in AI

Pythia (named after the oracle of Delphi) is a small machine-learning model inside Strojanka. It
runs entirely on your computer's processor: no graphics card, no internet AI service and no
extra cost. It retrains itself once a week in about half a minute on a laptop.

## What it predicts

For each stock, every day, Pythia gives two chances for the next 20 trading days:

- **Rise:** the price climbs by a set amount before it drops by half that amount.
- **Fall:** the price drops by that amount before it climbs by half of it.

The set amount depends on how much the stock usually moves: 2.5 times its typical daily move,
between 1.5% and 15%. These are the same rules the PEAK and SUPPORT alerts are graded by.

## What it looks at

About 25 measurements, using only information available that day:

- recent price changes (1 day to 6 months) and how jumpy the stock has been
- RSI, Bollinger bands, and the distance from the 20-, 50- and 200-day averages
- Strojanka's own swing analysis: the peak score, the support score and the current swing direction
- the position within the 52-week range, and trading volume compared with normal
- the whole market: how the average stock did over 1 and 3 months, how many stocks are above
  their 200-day average, and how this stock did compared with the market

It learns from all stocks on the arena's list at once (about 70 large US stocks, 10 years each,
around 160,000 stock-days). The method is gradient-boosted decision trees: many small "if this,
then that" rules, each correcting the previous ones. It is the same family of methods used by
XGBoost and LightGBM.

## How it is tested honestly

Pythia is tested **year by year on years it has never seen**. To predict 2022, it is trained only
on data up to about five weeks before 2022 began, so not even a 20-day outcome can leak in. Then
2023 is predicted by a model trained up to the end of 2022, and so on. Its track record is built
only from these blind predictions.

Two numbers describe it:

- **Ranking skill:** 0.50 means its chances are no better than guessing, and 1.0 would mean it
  always ranks the real winners first.
- **Its most confident calls:** how often its top 10% of rise calls came true, compared with
  how often a rise happens on an average day.

## How it did (October 2026)

| | Rise calls | Fall calls |
|---|---|---|
| Ranking skill | 0.51 | 0.52 |
| Most confident 10% right | 35% | 28% |
| On an average day | 37% | 29% |

**So far, Pythia has found no real edge.** Its confident calls were not better than average days.
A second test, which asked whether a stock would do better than the *other* stocks, also scored
0.50. On regular test cycles the same engine scores 0.996, so it does learn when there is
something to learn. On daily prices of large, heavily traded US stocks there simply is very
little to find over a few weeks. That agrees with what finance research has found for decades.

That is why Pythia **assists** rather than decides:

- its chances appear in reports and alerts, next to the normal "average day" chance, so you can
  see when it disagrees with the chart signals,
- `/pythia` shows its tested record and today's stocks with the highest rise and fall chances,
- two arena bots, **Pythia** and **Sibylla**, trade on its predictions. Like every bot they must
  pass the history test and 40 days of live trading before reporting in public. In October 2026
  both failed the history test, so they stay in training.

Pythia retrains every week. If the market changes and it starts to find a real edge, its tested
record and the oracle bots will show it.

## Commands

- `/pythia` in Discord, or `pythia` in the server window's console: the model's test record and
  today's top candidates.
- `/report stock:FCX` and alerts include a line such as:
  *Pythia (AI): 41% chance of a +6.8% rise before a -3.4% dip within 20 trading days (37% on an
  average day); 26% chance of the opposite (29%).*

Pythia is a probability tool, not financial advice.
