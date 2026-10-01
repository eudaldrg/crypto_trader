# 0001. Build against Kraken `level3`, Bitso `diff-orders` and Deribit FIX

- Status: proposed
- Date: 2026-10-01

## Context

The project needs real exchange market data, free or cheap, with no colocation,
to build a feed handler and order books against. What makes it worth building is
the variety of problems the feeds pose:

- **Order-by-order (L3) data**, where the book is rebuilt from individual
  add/modify/delete events by `order_id`. This is the harder and more
  representative book problem, and free L3 crypto data is rare: most providers
  charge for it. With only one L3 exchange, the "generic" L3 book would really
  be shaped around that exchange.
- **Aggregated (L2) data with its own sequencing and recovery**, closer to how
  institutional feeds behave.
- **A second protocol surface**, so the capture layer has to be generic rather
  than shaped around one exchange's transport and encoding.

Every client, parser and book policy built so far depends on which feeds were
picked.

## Options

- **Coinbase Advanced Trade:** public `level2` only. Coinbase Exchange's
  `full`/`level3` channels are genuine L3 but need authentication and an
  institutional Exchange account.
- **Paid aggregators (Tardis.dev, CoinAPI):** L3 and history, but paid, and a
  normalized format hides the wire problems this project is about. CoinAPI's L3
  covers only Bitso and Coinbase, in its own schema.
- **Bitso `diff-orders`:** genuine L3 (order id, price, amount, status), public,
  no credentials. A `sequence` that increments by one per message, so gaps are
  detectable, and recovery from a full-book REST snapshot spliced onto the
  buffered stream. Order updates are statuses (`open`/`cancelled`/`completed`)
  rather than add/modify/delete, and a price amend keeps the order id.
- **Bitfinex raw books (`R0`):** order-level and public, with optional sequence
  numbers and checksum, but only a 250-order window of the book.
- **Bitstamp `live_orders`:** order ids, EU-based; no snapshot on the stream and
  sequencing not confirmed from official docs.
- **Kraken WebSocket v2 `level3`:** genuine L3 (`order_id`, add/modify/delete)
  for any verified account, no paid tier. JSON over WebSocket; no sequence
  numbers, but a per-message checksum over the book.
- **Deribit WebSocket `book` (raw channel):** L2 with `change_id` /
  `prev_change_id` sequencing, authentication required.
- **Deribit FIX API:** L2 over FIX.4.4 on a retail-accessible testnet; tag=value
  encoding, session-level sequence numbers and heartbeats. A different protocol
  from everything else here, and the session layer can be hand-rolled rather
  than taken from QuickFIX, which is part of what this project demonstrates.
- **Deribit SBE over UDP multicast:** the closest to a real institutional feed,
  free of charge, but needs Equinix LD4 colocation or an AWS Transit Gateway
  peering request with real infrastructure cost.

## Decision

**Kraken `level3`, Bitso `diff-orders` and Deribit FIX market data.**

- Kraken gives the L3 book with a checksum and snapshot-on-subscribe, and no
  sequence numbers.
- Bitso gives a second L3 book shaped differently: sequence numbers with gap
  detection, recovery from a REST snapshot, and status-based order events. Two
  L3 exchanges are what show what a generic L3 book (`decisions/0013`) needs.
  Chosen over Bitfinex and Bitstamp because it is the whole book, sequenced, and
  confirmed live by a probe.
- Deribit FIX gives L2 with FIX session sequencing on a second transport and
  encoding.

None ranks above the others: they are exchanges posing different problems, and
the capture layer treats them the same way. (Earlier "primary"/"secondary"
wording meant only build order, which `decisions/0004` records.)

## Consequences

- None of the three offers a replay-from-sequence request, which shaped
  recovery (`decisions/0011`). Bitso's documented resync (buffer, fetch the REST
  snapshot, splice) could avoid a reconnect; under 0011 it reconnects like the
  others until that decision is revisited.
- Bitso is not built yet (#18).
- None gives redundant A/B feeds; demonstrating dedup and merge would need
  two connections to the same channel, documented as simulated.
- Deribit's WS `book` raw channel is used only as a recorded test fixture, for the
  `change_id`-sequenced L2 policy; nothing captures it live.
- Deribit SBE multicast is a stretch goal (`docs/architecture.md`).
- Wire facts, including what the probes confirmed, are in `exchanges/kraken.md`,
  `exchanges/bitso.md` and `exchanges/deribit.md`; the research behind the
  options is `docs/investigations/2026-09-23-free-l3-feeds.md`.

## Changes

- 2026-10-01: Bitso `diff-orders` added as a second L3 exchange, after research
  and a probe showed it is free, sequenced and the whole book.
