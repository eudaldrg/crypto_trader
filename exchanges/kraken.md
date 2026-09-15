# Kraken

See `decisions/0001-feed-source-selection.md` for why this feed was chosen.

## Endpoints

- REST: `https://api.kraken.com`
- WS v2 (L3 market data): `wss://ws-l3.kraken.com/v2`
- Instrument reference data: `GET /0/public/AssetPairs` (tick size, decimals
  — not carried inline in book messages, fetch/cache once at startup)

## Auth (required for `level3`, not for lower-detail public channels)

1. REST `POST /0/private/GetWebSocketsToken`, signed:
   - `nonce`: current time in ms.
   - `API-Sign` header = base64(HMAC-SHA512(secret, urlpath +
     SHA256(nonce + urlencoded(postdata)))), secret is base64-decoded first.
   - Response: `result.token`, used as the `token` field in the WS
     `subscribe` message (not a header — sent inside the WS payload).
2. Token is passed once per `subscribe` call; a reconnect needs a fresh
   token fetch. Kraken's docs describe the token as short-lived if unused
   (commonly cited as ~15 minutes) — **not yet independently confirmed by
   our own probe**, treat as short-lived and re-fetch on every (re)connect
   rather than caching across reconnects.
3. **Nonce must be strictly increasing per API key.** The probe's
   `str(int(time.time() * 1000))` (millisecond-resolution wall clock) is
   fine for a one-off probe but will collide under a fast automated
   reconnect loop (two calls within the same millisecond) — the real feed
   handler needs a monotonic counter or another guaranteed-increasing
   source, not a raw millisecond timestamp.
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
