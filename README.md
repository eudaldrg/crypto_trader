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
  market data over a hand-rolled FIX.4.4 session, journaled verbatim.
- **Order book engine**: reconstructs a full L3 book from that stream,
  designed for O(1)-style price→tick-index lookups rather than naive map
  lookups, with an eye toward sharding by security if/when parallelism is
  worth it.
- **Strategy hook**: the book publishes state (poll or callback-driven, TBD)
  for a strategy layer to consume — the strategy side itself is out of scope
  for now; this project is about the capture→book pipeline.

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
[`docs/questions/`](docs/questions/) what is still undecided.

## Building

CMake presets with Clang 21 and Ninja; see the Commands section of
[`AGENTS.md`](AGENTS.md).
