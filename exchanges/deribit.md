# Deribit

See `decisions/0001-feed-source-selection.md` for why this feed is a
secondary/future target (real market data here is L2, not L3).

## Surfaces

- **WS `book.*` channel**: public, no auth/KYC. Aggregated L2
  (`[operation, price, quantity]`). Carries real `change_id`/`prev_change_id`
  sequence numbers — genuine gap-detection problem, closer to CME-style than
  Kraken's feed.
- **FIX API** (the one this project targets, per `decisions/0002` — hand-rolled,
  not QuickFIX, as a deliberate protocol-level exercise): testnet host
  `fix-test.deribit.com:9881`, FIX.4.4. Also L2, not L3 — same granularity
  as `book.*`, just a different protocol surface (tag=value, session
  sequence numbers/heartbeats).
- **SBE over UDP multicast**: real L3-equivalent institutional feed, but
  gated behind Equinix LD4 colo or AWS Transit Gateway peering in specific
  regions — stretch goal, not in scope yet (see `decisions/0001`).

## FIX session (testnet, confirmed by probe)

### Logon (`35=A`)

- `49` (SenderCompID) = client ID, `56` (TargetCompID) = `DERIBITSERVER`.
- `98` (EncryptMethod) = 0, `108` (HeartBtInt) = 30.
- `553` (Username) = client ID.
- `96` (RawData) = `<timestamp_ms>.<base64(32 random bytes)>`.
- `554` (Password) = base64(SHA256(RawData + client_secret)).

### MarketDataRequest (`35=V`)

- `262` (MDReqID) = arbitrary request id.
- `263` (SubscriptionRequestType) = 1 (snapshot + updates).
- `264` (MarketDepth) = 0 (full book).
- `267`/`269` (NoMDEntryTypes / MDEntryType) = Bid (0) and Offer (1).
- `146`/`55` (NoRelatedSym / Symbol) = e.g. `BTC-PERPETUAL`.

### Responses

- `35=W` `MarketDataSnapshotFullRefresh` — full book snapshot.
- `35=X` `MarketDataIncrementalRefresh` — incremental updates, per-entry
  `MDUpdateAction`: `0` = New, `1` = Change, `2` = Delete.

Two independent sequencing layers to be aware of when building the real
client: FIX session-level `MsgSeqNum` (transport reliability —
`ResendRequest`/`SequenceReset` handle gaps at this layer) and the MD-level
snapshot/incremental structure above (business-level). Neither is the same
mechanism as Kraken's checksum approach.

## Confirmed by probe (2026-09-11)

`experiments/deribit_fix_probe.py` — real testnet session: `Logon` accepted,
`MarketDataSnapshotFullRefresh` then continuous
`MarketDataIncrementalRefresh` for `BTC-PERPETUAL`, real `MDUpdateAction`
values observed on the wire (not just documented).
