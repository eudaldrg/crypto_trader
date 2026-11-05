# crypto_trader

A from-scratch, latency-focused tick-to-strategy pipeline for crypto markets:
exchange feed capture → order book reconstruction → strategy hook, built the
way a real HFT feed handler is built rather than a typical retail trading bot.

**Status:** early stage. Capture from Kraken and Deribit, the journal, a golden
order book and journal replay exist; fast books, matching and strategies do not.
This README is deliberately just the pitch; `AGENTS.md` has the detail.

## What this is

- **Feed capture**: Kraken's L3 WebSocket order-book channel (genuine
  order-by-order data, `add`/`modify`/`delete` by `order_id`, not the
  aggregated price-level feeds most free crypto APIs offer) and Deribit's FIX
  market data over a hand-rolled FIX.4.4 session, journaled verbatim. Bitso's
  order-by-order feed is next, as a second, differently shaped L3 source.
- **Order books**: a deliberately simple golden book (L2 and L3) that rebuilds
  each exchange's book live from the feed and offline from a journal, and
  validates it (Kraken's per-message checksum, gaps, crossed books). It is the
  oracle that the fast, exchange-specific books will be checked against.
- **Deterministic replay**: `journal_replay` feeds a captured session through
  the same book code the live run used.

Still to come, in order ([`docs/roadmap.md`](docs/roadmap.md)): one normalized
market-data model across exchanges, a strategy that reads the books, order
entry against a simulated exchange, and a strategy trading in simulation. That
closed loop is v0. Fast books and latency work come after it, measured against
the golden book: correct first, then fast.

## Why

Built as a portfolio piece to demonstrate the systems-engineering side of HFT
work — concurrent feed ingestion, deterministic replay/journaling, low-latency
data structures, and profiling discipline — using real public market data
instead of a synthetic simulation.

## Design decisions

[`decisions/`](decisions/) holds the ADRs, one per expensive-to-reverse
decision (exchange feeds, the capture seam, the journal, threading, recovery and
failure containment, order-book design). [`docs/architecture.md`](docs/architecture.md)
covers how the pieces fit and the cheaper choices, and
[`docs/questions/`](docs/questions/) what is still undecided. What gets built
next, milestone by milestone, is [`docs/roadmap.md`](docs/roadmap.md).

## Building

CMake presets with Clang 21 and Ninja; see the Commands section of
[`AGENTS.md`](AGENTS.md).
