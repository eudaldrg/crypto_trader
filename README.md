# crypto_trader

A from-scratch, latency-focused tick-to-strategy pipeline for crypto markets:
exchange feed capture → order book reconstruction → strategy hook, built the
way a real HFT feed handler is built rather than a typical retail trading bot.

**Status:** early stage — environment and design decisions are being locked in
before any production code lands. See `decisions/` for the full rationale
behind each choice below; this README is deliberately just the pitch.

## What this is

- **Feed capture**: consumes Kraken's public L3 WebSocket order-book channel —
  genuine order-by-order data (`add`/`modify`/`delete` by `order_id`), not the
  aggregated price-level (L2) feeds most free crypto APIs offer.
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

See [`decisions/`](decisions/) for ADRs on feed source selection, the tooling
stack, and the development environment. Start there for the "why" behind
anything that looks like an unusual choice.

## Building

TBD — toolchain (CMake + Ninja + Clang, latest available) is being set up
now; instructions land here once there's something to build.
