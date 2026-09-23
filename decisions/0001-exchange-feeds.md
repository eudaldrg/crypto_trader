# 0001. Build against Kraken `level3` and Deribit FIX market data

- Status: proposed
- Date: 2026-09-23

## Context

The project needs real exchange market data, free or cheap, with no colocation,
to build a feed handler and order books against. What makes it worth building is
the variety of problems the feeds pose:

- **Order-by-order (L3) data**, where the book is rebuilt from individual
  add/modify/delete events by `order_id`. This is the harder and more
  representative book problem, and free L3 crypto data is rare: most providers
  charge for it.
- **Aggregated (L2) data with its own sequencing and recovery**, closer to how
  institutional feeds behave.
- **A second protocol surface**, so the capture layer has to be generic rather
  than shaped around one exchange's transport and encoding.

Every client, parser and book policy built so far depends on which feeds were
picked.

## Options

- **Coinbase Advanced Trade:** public `level2` only; public L3 is gone, available
  only through paid aggregators.
- **Paid aggregators (Tardis.dev, CoinAPI):** L3 and history, but paid, and a
  normalized format hides the wire problems this project is about.
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

**Kraken `level3` and Deribit FIX market data.** Kraken gives the L3 book problem
and its checksum; Deribit FIX gives L2 with FIX session sequencing on a second
transport and encoding. Neither ranks above the other: they are two exchanges
posing different problems, and the capture layer treats them the same way. (The
earlier "primary"/"secondary" wording meant only which one was built first,
which `decisions/0004` records.)

## Consequences

- Neither feed offers a replay-from-sequence request, which shaped recovery
  (`decisions/0011`).
- Neither gives redundant A/B feeds; demonstrating dedup and merge would need
  two connections to the same channel, documented as simulated.
- Deribit's WS `book` raw channel is used only as a recorded test fixture, for the
  `change_id`-sequenced L2 policy; nothing captures it live.
- Deribit SBE multicast is a stretch goal (`docs/architecture.md`).
- Wire facts, including what the probes confirmed, are in `exchanges/kraken.md`
  and `exchanges/deribit.md`.
