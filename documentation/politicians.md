# Politician stock trades

Members of the US Senate must publicly disclose their stock trades within 30–45 days, under the
STOCK Act. Strojanka checks for new disclosures once a day and can post them in your Discord
server and notify you personally about a specific politician.

## What's covered, and what isn't

- **US Senate only**, and only **electronic filings** (the kind senators file through the
  official web form). Older **paper filings** (scanned images) are skipped; there is no way to
  read their numbers automatically.
- **The US House of Representatives is not covered.** The House only publishes filings as PDFs,
  which would need a separate, larger piece of work (reading text out of PDF forms) that hasn't
  been built.
- The two well-known free trackers that used to cover this (HouseStockWatcher,
  SenateStockWatcher) have shut down, so Strojanka reads the Senate's own filing site directly.
- Because disclosures can legally appear up to ~45 days after the actual trade, **"new" means
  newly disclosed, not necessarily a trade from today.**

## Commands

- **`/politicians`** (or `politicians Pelosi` as text) — the most recent disclosed trades,
  everyone's or one politician's.
- **`monitor politician:<name>`** — personally watch one politician. You get a direct message
  the next time they disclose a trade. As text: `monitor politician Nancy Pelosi`.
- **`remove politician:<name>`** — stop watching them.

Every newly found trade is also posted automatically in the server's public channel, set the
same way as the trading-bot arena's channel:

```
admin channel <channel ID>
```

(Developer Mode on in Discord, then right-click the channel and **Copy Channel ID**.)

A public announcement looks like this:

> 🏛️ **Jane Doe** (Senator (DE)) disclosed a trade filed 2026-10-01:
> - Purchase **AAPL** (Apple Inc.), Self, $1,001 - $15,000, traded 2026-09-20.

## How it works, and its limits

Once a day, Strojanka visits the Senate's electronic filing search, lists every filing submitted
in roughly the last two months (long enough to catch late filings without re-reading everything),
and downloads the ones it hasn't seen yet. It skips paper filings and stores every new trade it
finds permanently, so nothing is announced twice.

**The Senate's site has bot protection (Akamai) that blocks some cloud and datacenter networks
outright.** It's expected to work normally from an ordinary home or office internet connection,
which is where Strojanka runs for almost everyone, but this couldn't be fully proven before
release. If disclosures stop appearing, check `strojanka.log` for a line mentioning
`politicians:` and an HTTP 403 — that means the connection was blocked, not that nothing new was
filed. In the server window's console, `politicians run` forces an immediate check and shows any
such error right away.

The trade data itself is a government filing read with ordinary text parsing, not a licensed
data feed: it should be accurate, but double-check anything important against the official
filing (the trade listing includes the filing itself when you ask the bot).

Not financial or legal advice — it simply republishes what politicians were required to disclose.
