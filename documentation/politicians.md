# Politician stock trades

Members of the US Congress must publicly disclose their stock trades within 30–45 days, under the
STOCK Act. Strojanka checks for new disclosures once a day, posts them in your Discord server and
can tell you personally when a politician you follow discloses a trade.

## Where the data comes from

- **House of Representatives:** the House Clerk's official site (disclosures-clerk.house.gov). It
  publishes a yearly list of filings and each trade report as a PDF, which Strojanka reads.
- **Senate:** the Senate's filing site (efdsearch.senate.gov). Its bot protection refuses
  connections from many networks, including, it seems, from outside the US: from a home
  connection in Hungary it answered "refused (HTTP 403)" every time. Strojanka still tries it
  each day and logs the refusal, but don't expect Senate trades unless the bot runs on a US
  connection.
- **Scanned paper filings are skipped** (about 15% of House filings): they are images, so there
  is no text to read.
- The free community trackers that used to cover this (HouseStockWatcher, SenateStockWatcher) have
  shut down, which is why Strojanka reads the official sites itself.
- Disclosures can legally appear up to ~45 days after the trade, so **"new" means newly
  disclosed**, not necessarily a trade from today.

## Commands

- **`/politicians`** (or `politicians Pelosi` as text): the most recent disclosed trades, newest
  filings first, everyone's or one politician's.
- **`monitor politician:<name>`**: personally watch one politician. You get a direct message when
  they disclose a new trade. Every word you type must appear in their name, so a last name is
  enough: `monitor politician Pelosi`, or `monitor politician Richard Allen` for Richard W. Allen.
- **`remove politician:<name>`**: stop watching them (type it the same way as when you added it).

Every new filing is also posted in the server's public channel, set the same way as the
trading-bot arena's channel:

```
admin channel <channel ID>
```

(Developer Mode on in Discord, then right-click the channel and **Copy Channel ID**.)

A filing is posted as one message with its trades (up to ten, then a pointer to `/politicians`):

> 🏛️ **Richard W. Allen**, Representative for GA12, disclosed 2 trades filed 2026-10-14:
> - Sale (Full) **AZO** (AutoZone, Inc. Common Stock (AZO)), Spouse, $15,001 - $50,000, traded 2026-09-22
> - Sale (Full) **COST** (Costco Wholesale Corporation - Common Stock (COST)), Spouse, $1,001 - $15,000, traded 2026-09-18

## How it works, and its limits

Once a day, Strojanka lists every trade report filed in roughly the last two months (long enough
to catch late filings) and downloads only the ones it hasn't read yet, a few seconds apart.

**The very first check stores what it finds without posting it.** Two months of filings are
several hundred trades, which would flood the channel; they are in `/politicians` straight away,
and every filing that appears after that first check is posted.

In the server window's console, `politicians run` checks right away and shows the result; the log
says what each source did (for example the Senate's refusal). From the command line,
`strojanka --politicians` does the same.

The trade data is read from the official filings with ordinary text parsing, not a licensed data
feed: it should be accurate, but check anything important against the filing itself on the
House Clerk's site.

Not financial or legal advice. It simply republishes what politicians were required to disclose.
