# Bitso

See `decisions/0001-exchange-feeds.md` for why this feed was chosen. Probe:
`experiments/bitso_l3_probe.py`; research and full probe output:
`docs/investigations/2026-09-23-free-l3-feeds.md`.

## Endpoints

- WebSocket: `wss://ws.bitso.com`, public, no credentials.
- REST order book: `GET https://api.bitso.com/api/v3/order_book/?book=<book>&aggregate=false`,
  public. Books are named like `btc_mxn`.

## `diff-orders` channel (genuine L3)

Subscribe: `{"action":"subscribe","book":"btc_mxn","type":"diff-orders"}`.
Ack: `{"action":"subscribe","response":"ok","time":...,"type":"diff-orders"}`.
Keepalive: `{"type":"ka"}`. Docs:
https://docs.bitso.com/bitso-api/docs/diff-orders-channel

Each message: `type`, `book`, `sequence`, `sent`, `payload` (an array, almost
always one entry). Each entry, confirmed by probe 2026-09-26:

| Key | Meaning |
|---|---|
| `o` | order id |
| `r` | price (decimal string) |
| `a` | amount (decimal string); absent on `completed` |
| `v` | value, price x amount; absent on `completed` |
| `t` | side: `0` bid, `1` ask (settled by probe; the 2023-03-02 changelog claims the opposite and is wrong for this channel) |
| `s` | status: `open`, `cancelled`, `completed` |
| `d` | order creation time, ms |
| `z` | undocumented; looks like an update time in ms, `0` on some cancels |

- **A price amend keeps the order id**: the same `o` arrives `cancelled` at the
  old price and `open` at the new one, with the same `d`. There is no explicit
  modify. In one minute of `btc_mxn`, 148 of 177 order ids appeared more than
  once.
- Partial fills arrive as an `open` with a smaller `a`; a full fill as
  `completed`.

## Sequencing and recovery

- `sequence` increases by exactly one per message (372 consecutive messages, no
  gaps, in the probe). A jump means a message was lost.
- Documented resync (https://docs.bitso.com/bitso-api/docs/general): subscribe
  and buffer, fetch the REST snapshot, drop buffered messages with
  `sequence <= snapshot.sequence`, apply the rest. The probe's snapshot sequence
  sat a few messages after the first buffered one, so the splice works.
- The REST snapshot (`aggregate=false`) is the whole book, not a depth window:
  3,497 bids and 4,225 asks for `btc_mxn` on 2026-09-26, each with `oid`,
  `price`, `amount`, `book`, plus a top-level `sequence` and `updated_at`.
- No checksum.
