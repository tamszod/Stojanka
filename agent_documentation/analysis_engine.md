# Analysis engine

Files: `src/core/analysis/{indicators,zigzag,analyzer,calibrator,walk_forward,classifier}.*`.
Input: daily adjusted `Bar`s (oldest first). Everything is per symbol.

## 1. Swings (zigzag.cpp)
Online ZigZag on closes with reversal threshold `swing_pct`. A pivot is confirmed only after
price reverses `swing_pct` from the extreme, so state at bar i has no look-ahead.
`direction()`: +1 rising leg (last pivot = low, watching for PEAK), -1 falling leg (watching for SUPPORT).
`extreme()`: running max (rising) / min (falling) close since the last pivot.

## 2. Score per bar (analyzer.cpp `analyze_series`)
Warm-up 30 bars. Uses the last 8 pivots for typical swing amplitudes (median of up/down legs) and
the last 3 highs/lows for resistance/support (fallback: 60-bar max/min).
PEAK score (rising leg), each component in [0,1] via `smoothstep`:
```
0.30 * amp   smoothstep(0.45, 1.0, gain_since_low / typical_up)
0.20 * level 1 - |close/resistance - 1| / (0.5*swing_pct)
0.25 * rsi   smoothstep(50, rsi_high, RSI14)
0.10 * band  smoothstep(0.6, 1.0, %B)
0.15 * turn  smoothstep(0, 0.35*swing_pct, 1 - close/extreme)
```
SUPPORT mirrors it (drop vs typical_down, support level, RSI toward rsi_low, %B toward 0, bounce from min).
Alert when `score >= threshold + bias` (`peak_cutoff()`, clamped to [0.35, 0.97]); one per leg.

## 3. Grading (`evaluate_outcome`, triple barrier)
For a call at bar i, entry price p, `move = move_pct`:
PEAK correct if low ≤ p(1-move) before high ≥ p(1+move/2) within `horizon_bars` (20); the adverse
barrier is checked first on the same day; neither = EXPIRED (counts as not correct); data ends = PENDING.
`move_pct = clamp(2.5 * daily_vol, 1.5%, 15%)`, `swing_pct base = clamp(4 * daily_vol, 3%, 30%)` (`default_params`).

## 4. Calibration (calibrator.cpp) — per stock, daily
Grid: swing factor {0.6,0.8,1,1.3,1.7} × RSI pairs {(62,38),(68,32),(74,26)} × thresholds 0.50..0.85 step .05.
Per side: simulate one-call-per-leg, recency weight 0.5^(age/250 bars), smoothed precision (C+1)/(W+2),
coverage vs. hindsight pivots, `objective = precision * (0.5 + 0.5*coverage)`. Swing factor is shared
(best peak obj + best support obj); RSI + threshold chosen per side. Biases carried from previous params.

## 5. Learning from live mistakes
Graded live signals → `adjust_bias(bias, last 10 resolved of that side)`: precision < 50% → +0.03 (max +0.15);
≥ 75% → −0.01 (min −0.05). Triggers recalibration that cycle.

## 6. Honest accuracy (walk_forward.cpp)
Every 20 bars: calibrate on the previous ≤520 bars, analyse the next 20 blind, grade calls, and also grade
"a call on every day" → baseline. Stored in `stock_params.backtest_*` / `baseline_*` daily (10y history),
shown in `/report` and in alerts. `has_proven_edge`: ≥8 calls and hit rate ≥ baseline + 5 points.

## 7. Categories (classifier.cpp)
On the last 252 bars: annual_vol = daily_vol·√252; annual_trend = exp(slope·252)−1 of log-price regression; r²;
swings_per_year (pivots at clamp(4·daily_vol)). Order of rules: HighVolatility (vol>60%) → Uptrend (trend>+20%, r²>0.5)
→ Downtrend (trend<−20%, r²>0.5) → Defensive (vol<18%) → Cyclical (≥4 swings/yr, r²<0.5) → Mixed; <120 bars = Unknown.
Category is descriptive only (it does not change parameters yet).
`short_term_trend`: 20-bar log regression change vs. ±max(1.5%, 0.35·daily_vol·√20).

## 7b. News overlay (core/news, applied in MonitorService::refresh)
Not part of `analyze_series` and not in walk-forward/backtests (no news history: free RSS reaches ~2–6 weeks).
Final score = chart score ∓ w·tone (see architecture.md step 5b), w learned live from `signals.news_sentiment`
vs outcome. Red-flag headlines veto SUPPORT alerts. `news_items` accumulates history; once months of it exist,
a news-aware walk-forward becomes possible (align headline timestamps to bars, no look-ahead).

## 7c. Arena (core/arena) — strategies as paper-trading bots
Separate from PEAK/SUPPORT alerts. See architecture.md for the roster and gates. First real run (2026-10-12, 70-stock
default universe, 2017-11 → 2026-10): Atlas $1,000 → $4,983 (Sharpe 0.98, DD 32%); passed: Athena (Sharpe 0.67),
Minerva (0.78), Mars (1.00, DD 35%), Diana (0.91, DD 26%); failed: Ares (Sharpe 0.51, DD 42%), Hermes (DD 43% despite
$20.9k), Mercurius (DD 37%), Artemis (lost money in first third), Nemesis (0.34), Iustitia (0.50).
**Survivorship bias:** the universe is today's large caps, so absolute backtest numbers are inflated (momentum most of all);
compare against Atlas and rely on the live proving stage. A point-in-time universe (historical index members) would fix it.

## 7d. Pythia (core/ml) — the AI model
CPU-only GBDT (no GPU, no external libs, no paid services). Results 2026-10-12 (157,640 stock-days, 7 unseen test
years 2020–2026, 118,840 OOS predictions): AUC up 0.51 / down 0.52; top-decile hit 35% vs 37% base (up), 28% vs 29%
(down). Experiment with a cross-sectional label (beat the median stock over 20 d): AUC 0.50 — rejected and removed.
Sanity: the same learner reaches AUC 0.94 on a synthetic linear signal and 0.996 on synthetic cycles (unit tests),
so the null result reflects the data, not a bug. Oracle bots: Pythia Sharpe 0.33, Sibylla 0.17 → stay in training.
Ideas if revisited: longer horizons, fundamentals/earnings dates (free sources needed), a point-in-time universe.

## 7e. Politicians (core/politicians) — Congress trade disclosure tracking
Not a prediction model: it republishes STOCK Act Periodic Transaction Reports from the official sites, since the
free community trackers (HouseStockWatcher, SenateStockWatcher) have shut down and no clean official API exists.
**House** (main source): yearly ZIP index + one generated PDF per report, read with the small `pdf_text` reader
(first live run 2026-10-16: 74 filings in 60 days, 664 trades, 11 scanned paper filings skipped, 0 failures; about
15% of reports are RC4-"encrypted" with an empty password and are decrypted). **Senate**: HTML tables, but
efdsearch.senate.gov's Akamai protection answered 403 to a Hungarian home connection (issue #4) whatever the
headers or TLS stack, so it likely serves US connections only; it is still tried daily and the refusal logged.
Scanned paper filings are skipped in both chambers (would need OCR). Disclosures may legally lag the trade by ~45
days, so "new" means newly disclosed. The parser targets the Clerk's generator (fixed column positions, Georgia
small-caps glyph ids); if the layout changes, `parse_house_ptr` reports "no transaction table" instead of guessing —
check against the saved fixtures in `tests/fixtures/`.

## 8. Measured performance (2026-10-10, `strojanka --backtest`, 10y Yahoo daily data)
| Set | PEAK right | random | SUPPORT right | random |
|---|---|---|---|---|
| 16 "train" tickers (FCX AAPL KO TSLA MSFT XOM JPM NVDA PFE CAT DIS INTC T NEM AMD WMT) | 25% (105/421) | 28% | 41% (173/425) | 38% |
| 20 holdout tickers (GOOGL AMZN BA GE MRK CVX BAC NKE SBUX MU F GM DD HD PEP ORCL QCOM VZ COP SLB) | 27% | 29% | 33% | 37% |
| synthetic sine cycle (unit test) | 90% | — | 100% | — |

Conclusion: the machinery works on truly cyclical data, but on real stocks the v0.1.0 score has **no
out-of-sample edge** on average (some stocks, e.g. KO, do show one). Tried and rejected (no holdout gain):
Wilson-lower-bound selection, 100-day SMA trend gate, mandatory turn confirmation, fixed un-calibrated params.
In-sample calibration accuracy is heavily overfit (≈60%) — never show it to users.
Promising next steps: pooled cross-stock model (logistic regression / gradient boosting on the score
components + trend/volume/market-regime features) trained with walk-forward; per-category priors;
using SPY regime; alert gating on `has_proven_edge`. Always validate with `--backtest` on the holdout set.
