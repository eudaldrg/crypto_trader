# Kraken

See `decisions/0001-feed-source-selection.md` for why this feed was chosen.

## Endpoints

- REST: `https://api.kraken.com`
- WS v2 (L3 market data): `wss://ws-l3.kraken.com/v2`
- Instrument reference data: `GET /0/public/AssetPairs` (tick size, decimals
  — not carried inline in book messages, fetch/cache once at startup).
  Unauthenticated, no signing. Response is
  `{"error":[], "result":{"<REST name>":{...}}}`; the fields that matter are
  `altname` (`"XBTUSD"`), `wsname` (`"XBT/USD"`), `pair_decimals` (price
  decimals), `lot_decimals` (quantity decimals) and `tick_size` (a decimal
  *string*, e.g. `"0.1"` — keep the text, don't round-trip it through a
  double before the order book has decided how it represents prices).
  Confirmed live 2026-09-16: 1450 pairs returned for an unfiltered request.

## Symbol naming: `XBT` (REST) vs `BTC` (WS v2)

Confirmed live on 2026-09-16, and a genuine trap: REST `AssetPairs` reports
`XXBTZUSD` with `wsname` `"XBT/USD"`, but the WS v2 `level3` channel
subscribes to the same instrument as `"BTC/USD"` (that is what
`experiments/kraken_l3_probe.py` actually sends, successfully). So Kraken's
own `wsname` field does *not* give the string WS v2 wants for bitcoin.
Anything joining reference data to feed symbols has to treat `XBT` and `BTC`
as the same asset rather than trusting `wsname` verbatim.

## Auth (required for `level3`, not for lower-detail public channels)

1. REST `POST /0/private/GetWebSocketsToken`, signed:
   - `nonce`: a strictly increasing integer (see 3 below for what the C++
     client actually uses).
   - `API-Sign` header = base64(HMAC-SHA512(secret, urlpath +
     SHA256(nonce + urlencoded(postdata)))), secret is base64-decoded first.
   - Response: `result.token` plus `result.expires`, used as the `token`
     field in the WS `subscribe` message (not a header — sent inside the WS
     payload).
2. Token is passed once per `subscribe` call; a reconnect needs a fresh
   token fetch. **Confirmed live on 2026-09-16** (`src/feed_handler/kraken`
   signing implementation, real signed call): the response carries
   `"expires": 900`, i.e. the commonly cited ~15 minutes is correct and is
   stated by the API itself rather than only by the docs. Still re-fetch on
   every (re)connect rather than caching across reconnects — the window is
   short enough that a stale token is a live failure mode.
3. **Nonce must be strictly increasing per API key.** The probe's
   `str(int(time.time() * 1000))` (millisecond-resolution wall clock) is
   fine for a one-off probe but will collide under a fast automated
   reconnect loop (two calls within the same millisecond). The C++ client
   uses `max(current_microseconds_since_epoch, last_nonce_used + 1)`, which
   stays strictly increasing across same-microsecond bursts *and* backwards
   NTP steps. The high-water mark is **also persisted** to a small local file
   (`state/kraken-nonce.state` next to `journal/`, one decimal number, written
   via temp file + rename after every signed call), so a restart cannot rewind
   it either: on startup the next nonce is
   `max(persisted + 1, current_microseconds)`. Neither source is trusted
   alone — the file cannot hold the nonce back once the clock has moved past
   it, and the clock cannot regress below what the file has seen.

   The file is defense in depth, never a startup dependency: a missing file
   (ordinary first run), unparseable contents and an unwritable directory all
   fall back to plain clock-only behavior with a warning. Nonce values are not
   secret, so unlike the key/secret/token they are logged and stored in the
   clear. The file is per (API key, machine): **two processes sharing one API
   key still need a shared mark** and are not made safe by this.
4. **Never journal the token or any raw `subscribe` payload** — the token
   travels inside the WS message body (see `decisions/0004`'s journal-format
   section), not a header, so a naive "journal everything on this
   connection" implementation would leak it into an archived file. Journal
   inbound messages only; if outbound traffic is ever logged for debugging,
   redact the token field first.
5. Unbounded reconnect attempts will eventually hit Kraken's REST rate
   limits on `GetWebSocketsToken` — reconnect logic needs exponential
   backoff with jitter, not a tight retry loop.

## `level3` channel

Subscribe:
```json
{
  "method": "subscribe",
  "params": {
    "channel": "level3",
    "symbol": ["BTC/USD"],
    "snapshot": true,
    "token": "<ws token>"
  }
}
```

- `snapshot: true` returns a full order-by-order snapshot first, then
  incremental updates, all on the same connection — no separate snapshot
  endpoint/connection to reconcile against.
- Incremental messages carry `event`: `add` / `modify` / `delete`, each with
  `order_id`, `limit_price`, `order_qty` — genuine L3, not aggregated by
  price level. **The events are nested inside the per-symbol `data` entry's
  own `bids`/`asks` arrays**, not at the top level, and each one carries its
  own `timestamp` (the order's, which for an `add` can be minutes older than
  the message):
  ```json
  {"channel":"level3","type":"update","data":[{"checksum":4239577768,
   "symbol":"BTC/USD","timestamp":"2026-09-15T23:41:47.461426256Z",
   "bids":[],"asks":[{"event":"delete","order_id":"OOZP53-FUXQT-BM3262",
   "limit_price":75623.0,"order_qty":0.99200222,
   "timestamp":"2026-09-15T23:41:47.461426256Z"}]}]}
  ```
  One message can carry several events, on both sides at once.
- **No sequence numbers on the feed at all.** Kraken's docs state
  explicitly that no sequencing is required — TCP ordering is the ordering
  guarantee. There is no lagging-snapshot-vs-incremental-feed reconciliation
  problem the way there would be on a UDP multicast setup.
- **Every snapshot and update message carries a `checksum` field** (e.g.
  `"checksum":4239577768`), confirmed empirically on 2026-09-11, not just
  documented. It sits **inside each `data[]` entry**, next to that entry's
  `symbol` and `timestamp`, not at the top level of the message — it is
  per-symbol, which is what makes it work on a connection carrying several
  symbols. This is a real self-consistency mechanism: recompute the same
  checksum over the locally reconstructed book after applying an update and
  compare. Only usable once an order book actually exists to recompute
  against (checksum is defined over reconstructed top-of-book state, not
  derivable from a single message in isolation).

## Connection-level traffic (confirmed live 2026-09-16)

Captured by `kraken_feed_handler` over a 45s `BTC/USD` `level3` session
(2205 inbound messages, all journaled verbatim):

1. **The first inbound message on every connection is unsolicited**: a
   `status` message that arrives before the subscribe response, carrying
   `version`, `system`, `api_version`, a `connection_id`, and an
   `upcoming_maintenance` array of scheduled-maintenance windows. Anything
   assuming the first message after connecting is a subscribe reply is
   wrong.
2. The subscribe reply is a method response, not a channel message:
   ```json
   {"method":"subscribe","result":{"channel":"level3","snapshot":true,
    "symbol":"BTC/USD"},"success":true,"time_in":"...","time_out":"..."}
   ```
   A rejection has the same shape with `"success":false` and an `"error"`
   string. **A rejected subscribe does not close the connection** — the
   socket stays open and simply never delivers data, so it has to be
   detected from the message, not from a disconnect.
3. `{"channel":"heartbeat"}` arrives about once a second (45 in 45s) and is
   the only traffic on an idle connection, which is what makes an
   application-level staleness timeout usable at all (`decisions/0004`).
4. Message rate for `BTC/USD` `level3` alone: roughly 50 messages/second
   (2158 update messages in 45s: 1692 `add`, 1053 `delete`, 36 `modify`
   events across them), around 800 KiB of journal for 45 seconds.

## Multi-symbol subscribe (confirmed live 2026-09-20)

`params.symbol` is an array, and one `subscribe` message carries all of them.
Confirmed with `["BTC/USD", "ETH/USD"]` on the production feed (30 s, via the
feed handler's `--config` mode):

- **One method response per symbol**, each carrying that symbol in
  `result.symbol` (`{"method":"subscribe","result":{"channel":"level3",
  "snapshot":true,"symbol":"ETH/USD"},"success":true,...}`), not one response
  for the whole request. A client counting acknowledgements has to expect N,
  and can read the symbol from each to name the missing one.
- **One snapshot per symbol**, then updates for both interleaved on the same
  connection, each `data[]` entry carrying its own `symbol` and `checksum`.
- Documented limits, not exercised: at most 200 symbols per connection, and a
  depth-weighted subscribe budget (5 per symbol at depth 10, against 200/s on a
  standard account).

## Recovery

On disconnect: reconnect, fetch a fresh WS token, resubscribe with
`snapshot: true` again. There is no "resume from sequence N" request —
the fresh snapshot *is* the recovery mechanism.

## Confirmed by probe (2026-09-11)

`experiments/kraken_l3_probe.py` — real snapshot + incremental traffic for
`BTC/USD` observed end-to-end, `order_id`-level `add`/`delete` events and
the `checksum` field confirmed present on every update message.

## Level3 checksum algorithm and client-side depth truncation (2026-09-17)

The checksum *field's existence* was confirmed above; the algorithm that
computes it was not documented anywhere in this repo until now, and two
things about it are easy to get wrong even with Kraken's own docs open.
Sourced from
[docs.kraken.com/api/docs/guides/spot-ws-l3-v2](https://docs.kraken.com/api/docs/guides/spot-ws-l3-v2/)
and
[docs.kraken.com/api/docs/websocket-v2/level3](https://docs.kraken.com/api/docs/websocket-v2/level3/),
and verified against a real ~2-minute `BTC/USD` capture via the real
`kraken_feed_handler` binary (not a Python probe), replayed by
`src/order_book/tests/kraken_capture_replay_test.cpp` — 6496 wire records
captured, of which 6362 carry a `level3` checksum (1 snapshot + 6361
updates; the rest is non-`level3` control traffic), zero checksum
mismatches across all of them (see decisions/0006's plan, task T7).

**Algorithm**: for each side, take the top 10 price levels (asks
ascending, bids descending), and within each level iterate its resting
orders. For each order, concatenate its price then its quantity with the
decimal point removed and leading zeros stripped, in that order.
Concatenate every order's token, asks first then bids, and take the
standard CRC-32 (the zlib/ISO-HDLC/PKZIP variant — reflected, polynomial
`0xEDB88320`, init/final XOR `0xFFFFFFFF`) of the resulting ASCII string.
Verified byte-for-byte against Kraken's own worked example before
trusting it (`src/order_book/tests/kraken_checksum_test.cpp`).

**Client-side depth truncation is mandatory, and Kraken does not do it
for you.** Kraken's own docs: *"After each update, the book should be
truncated to your subscribed depth, there will be no `delete` event for
price levels that fall out of scope."* No `depth` parameter is sent on
subscribe (`kraken_ws_client.cpp`), so the default (10) applies — the
same window the checksum itself uses. Two consequences that only showed
up against real captured data, not in a hand-written unit test:

1. **An order that drops out of the tracked window and later re-enters
   gets a fresh `add` event carrying the *same* `order_id`.** Kraken's
   docs confirm the mechanism: *"If a price level is removed from the
   subscribed levels ... then all orders in the next available level will
   generate an add event."* A book that doesn't truncate itself keeps a
   stale copy of that order around, so the "fresh" add duplicates it
   instead of representing a genuine re-entry — this corrupts the
   checksum in a way that's invisible until the stale order's level
   happens to become checksum-relevant again, which can be hundreds of
   messages later (first observed at message 116 of the capture, several
   minutes of "everything looks fine" before it surfaced).
2. **Multiple orders entering the tracked window in the same message are
   not guaranteed to be listed in true arrival order.** When a level
   drops out and the next one becomes visible, Kraken announces every
   resting order at that level via `add` — but their position in the
   JSON array does not reliably match their own `timestamp` field's
   order. Applying them in array order silently gets queue priority (and
   the checksum) wrong; sorting by each entry's own `timestamp` before
   applying fixes it. Confirmed by direct comparison against the real
   capture (first divergence at message 775 once (1) was already fixed).
