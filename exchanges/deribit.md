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

- **There is no ack for the MarketDataRequest.** The `35=W` snapshot *is* the
  acknowledgement; the only negative answer is `35=Y`
  `MarketDataRequestReject`. So "subscribed but no data" and "silently
  rejected" are told apart by the arrival of the snapshot, not by a reply.
- `35=W` `MarketDataSnapshotFullRefresh` — full book snapshot.
- `35=X` `MarketDataIncrementalRefresh` — incremental updates, per-entry
  `MDUpdateAction`: `0` = New, `1` = Change, `2` = Delete.
- Both carry, above the entry group: `55` (Symbol), `231`
  (ContractMultiplier), `746`, and Deribit's own custom tags `100087`,
  `100090` (plus `100092`/`100093` on `35=W`), then `262` (MDReqID) and `268`
  (NoMDEntries). Each entry is `269`/`270`/`271`/`272`
  (MDEntryType/Px/Size/Date), preceded by `279` (MDUpdateAction) on `35=X`.

### Session keepalive and sequencing

- `HeartBtInt(108)=30` is what the accepted Logon negotiates, so the client
  owes a `Heartbeat` (`35=0`) every 30s of outbound silence, and owes an
  immediate `Heartbeat` echoing `TestReqID(112)` whenever the server sends a
  `TestRequest` (`35=1`). An unanswered `TestRequest` ends the session.
- `MsgSeqNum(34)` starts at 1 in each direction for each new session. The
  probe sent no `ResetSeqNumFlag(141)` and Deribit accepted a session
  beginning at 1 regardless, so a reconnect just starts counting again.
- The FIX envelope itself (`8=FIX.4.4`, `9=<BodyLength>`, `10=<CheckSum>`) is
  standard FIX.4.4, not a Deribit quirk — see
  `src/feed_handler/fix/fix_message.h` for the exact arithmetic. One value
  Deribit actually sends that the arithmetic has to survive: `RawData(96)` and
  `Password(554)` are base64 and routinely contain `=`, so a field is split on
  its *first* `=` only.

Two independent sequencing layers to be aware of when building the real
client: FIX session-level `MsgSeqNum` (transport reliability — standard FIX
repairs gaps here with `ResendRequest`/`SequenceReset`, though this project
deliberately does not; see `decisions/0004`) and the MD-level
snapshot/incremental structure above (business-level). Neither is the same
mechanism as Kraken's checksum approach.

## Confirmed by probe (2026-09-11)

`experiments/deribit_fix_probe.py` — real testnet session: `Logon` accepted,
`MarketDataSnapshotFullRefresh` then continuous
`MarketDataIncrementalRefresh` for `BTC-PERPETUAL`, real `MDUpdateAction`
values observed on the wire (not just documented).

## Confirmed by the C++ client (2026-09-16)

`deribit_feed_handler`, 45s against the live testnet: 432 inbound messages
journaled — 1 `35=A`, 1 `35=W`, 429 `35=X`, 1 `35=0` — with zero
session-level gaps and zero envelope (`BodyLength`/`CheckSum`) failures across
the whole capture. What that run added to the facts above:

- **Deribit's own `Logon` reply carries `95` (RawDataLength), `96` (RawData)
  and `554` (Password)** — its own challenge material, not an echo of ours.
  An *inbound* Logon is therefore credential-shaped too, so the "never log
  Logon/Password field values" rule applies to received messages, not only to
  sent ones.
- **Our Logon omits `95` (RawDataLength) and Deribit accepts it.** The probe
  never sent it either; the field is not required on the client side.
- **The full `BTC-PERPETUAL` snapshot was 5424 bytes** (`268` = 98 entries),
  i.e. larger than a typical single `recv()`. The very first market-data
  message therefore exercises reassembly across reads — a framer that assumed
  one message per read would break immediately, not eventually.
- **Incremental refreshes carry 1–9 entries** (`268`), most commonly 1 or 2 —
  they are not one-update-per-message.
- **The server heartbeat interval matches the negotiated `HeartBtInt`=30s**
  (one `35=0` in a 45s session, no `TestReqID` on it). No `35=1` `TestRequest`
  arrived in that window, so the TestRequest-answering path is unit-tested but
  has not yet been seen live.
