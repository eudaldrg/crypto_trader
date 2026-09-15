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
   NTP steps. No cross-process high-water mark is persisted: v1 assumes one
   continuously running process per API key, so a restart that rewinds the
   clock, or a second process sharing the key, would still need one.
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
  price level.
- **No sequence numbers on the feed at all.** Kraken's docs state
  explicitly that no sequencing is required — TCP ordering is the ordering
  guarantee. There is no lagging-snapshot-vs-incremental-feed reconciliation
  problem the way there would be on a UDP multicast setup.
- **Every update message carries a `checksum` field** (e.g.
  `"checksum":1671312190`), confirmed empirically on 2026-09-11, not just
  documented. This is a real self-consistency mechanism: recompute the same
  checksum over the locally reconstructed book after applying an update and
  compare. Only usable once an order book actually exists to recompute
  against (checksum is defined over reconstructed top-of-book state, not
  derivable from a single message in isolation).

## Recovery

On disconnect: reconnect, fetch a fresh WS token, resubscribe with
`snapshot: true` again. There is no "resume from sequence N" request —
the fresh snapshot *is* the recovery mechanism.

## Confirmed by probe (2026-09-11)

`experiments/kraken_l3_probe.py` — real snapshot + incremental traffic for
`BTC/USD` observed end-to-end, `order_id`-level `add`/`delete` events and
the `checksum` field confirmed present on every update message.
