# 0001. Feed source selection: Kraken L3 WebSocket

## Context

The goal was genuine order-by-order (L3) market data — individual order IDs
with add/modify/delete/execute events — rather than aggregated price-level
(L2) depth, since reconstructing a real order book from raw order events is
the harder and more representative engineering problem, and a better
portfolio story than price-level aggregation.

Free L3 crypto data is rare; most providers (CoinAPI, Tardis.dev) charge for
it. Options investigated:

- **Coinbase Advanced Trade**: public `level2` channel only. Coinbase has
  quietly deprecated public L3 — order-by-order data is only available via
  paid aggregators now.
- **Deribit**: public `book.*` WebSocket channel is free, no auth/KYC
  required, but aggregated L2 (`[operation, price, quantity]`), not
  order-level. It does carry `change_id`/`prev_change_id` sequence numbers —
  a genuine gap-detection problem to solve, closer to a CME-style feed than
  Kraken's (see below). Deribit also runs a real UDP multicast feed
  (SBE-encoded, same style CME uses) — free of charge, but gated behind
  either Equinix LD4 colocation or an AWS Transit Gateway peering
  arrangement in eu-west-2/ap-northeast-1 requiring an access request and
  non-trivial AWS infrastructure cost. Deemed too much setup/cost overhead
  to be the starting point; kept as a stretch goal (see Consequences).
- **Kraken**: public WebSocket v2 `level3` channel is free, requires only an
  authenticated API key (any normal verified Kraken account, no paid tier,
  no KYC beyond normal account verification), and gives genuine order-level
  data: `order_id`, `limit_price`, `order_qty`, `event` (`add`/`modify`/
  `delete`). Instrument reference data (tick size, decimals) comes via REST
  `GET /0/public/AssetPairs`, not inline in book messages — pull/cache at
  startup.

## Decision

Start with **Kraken's public L3 WebSocket `level3` channel** as the feed
source.

## Consequences

- Kraken's feed arrives over a single WebSocket (TCP) connection with **no
  sequence numbers** — Kraken's own docs state "no sequencing is required."
  Unlike a CME-style UDP multicast setup, there is no separate lagging
  snapshot feed to reconcile against an ahead incremental stream: TCP
  ordering already solves what sequence-number gap detection solves at the
  exchange-feed level. Book "readiness" reduces to "have I received a
  snapshot since my last (re)connect," not an ongoing catch-up state
  machine.
- There is no native A/B redundant feed (a real HFT pattern for covering
  independent packet loss on separate multicast groups) — Kraken gives one
  logical feed. If demonstrating dedup/merge logic matters later, redundancy
  would have to be manufactured (two independent connections to the same
  channel), and documented as simulated rather than exchange-provided.
- Deribit's `book` channel (with real `change_id`/`prev_change_id` gap
  detection) remains a good secondary target if a second, differently-shaped
  feed-handling problem is wanted later.
- Swapping the transport for Deribit's SBE-over-UDP-multicast feed (the
  closest available analog to a real institutional/colo setup) is an
  explicit stretch goal once the core pipeline (capture → sequencer → order
  book) works end-to-end against Kraken — not a blocker on getting started.
- Deribit also runs a retail-accessible **FIX API** (`fix-test.deribit.com`
  for testnet, no colocation needed) — a genuinely different protocol
  surface (tag=value encoding, session-level sequence numbers/heartbeats)
  worth targeting for the multi-exchange interface exercise, but its market
  data is still aggregated L2 (`MDUpdateAction`: New/Change/Delete per price
  level via `MarketDataSnapshotFullRefresh`/`MarketDataIncrementalRefresh`),
  same granularity as the WS `book` channel — it does not give L3.

## Empirical validation (2026-09-11)

Both a Kraken `level3` WS probe and a Deribit testnet FIX probe were
actually run (throwaway scripts in `experiments/`, not the real design) and
confirmed real traffic end-to-end:

- **Kraken**: real order-book snapshot + incremental updates for `BTC/USD`,
  genuine `order_id`-level entries with `event: add`/`delete`. One thing not
  fully appreciated from the docs alone: **every level3 update message
  carries a `checksum` field**, e.g. `"checksum":1671312190`. So although
  there's no monotonic sequence number, there *is* a concrete
  self-consistency mechanism available — recompute the same checksum over
  the locally-reconstructed book after applying each update and compare
  against the message's checksum to detect a desynced book. Worth using
  this rather than assuming TCP ordering alone is a sufficient correctness
  check.
- **Deribit FIX (testnet)**: confirmed `Logon` → `MarketDataSnapshotFullRefresh`
  (`35=W`) → continuous `MarketDataIncrementalRefresh` (`35=X`) for
  `BTC-PERPETUAL`, with real `MDUpdateAction` values (0=New, 1=Change,
  2=Delete) per entry — confirms the L2-not-L3 granularity above with real
  traffic, not just docs.
