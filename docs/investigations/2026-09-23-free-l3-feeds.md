# Free order-by-order (L3) crypto feeds (2026-09-23)

Question: is there a second free, retail-accessible exchange with genuine L3 data,
so the golden L3 book is proven generic rather than Kraken-shaped? Prompted by a
CoinAPI blog post claiming L3 for Bitso and Coinbase. Web research only, from
official docs where reachable; "unverified" marks what could not be confirmed.

## CoinAPI

- L3 "only available for selected exchanges: BITSO and COINBASE"
  (https://www.coinapi.io/products/market-data-api/faq).
- WebSocket `book_l3`: `symbol_id`, `sequence`, `time_exchange`, `time_coinapi`,
  `is_snapshot`, and orders with `id`, `price`, `size`, `update_type`
  (ADD/UPDATE/SUBTRACT/DELETE/MATCH). Order ids are null for L2-only venues
  (https://www.coinapi.io/products/market-data-api/docs/websocket/messages).
- **Normalized**, CoinAPI's own schema, not the exchange's bytes.
- **Not free**: $25 of signup credits with a verified payment method, then metered
  or $79-$599/month (https://www.coinapi.io/products/market-data-api/pricing). L3
  is not mentioned on the pricing page; whether the credits cover it is
  unverified. One search snippet claimed `book_l3` updates "every 1 second"
  (throttled, not tick-by-tick); unverified.

Verdict: paid and normalized, the two reasons `decisions/0001` gives for not using
aggregators.

## Direct exchange feeds

- **Bitso `diff-orders`** (`wss://ws.bitso.com`): per order `o` (id), `r` (price),
  `a` (amount), `t` (side), `s` (open/completed/cancelled); partial fills arrive
  as updated amounts (https://docs.bitso.com/bitso-api/docs/diff-orders-channel).
  A per-message `sequence` incrementing by one; a gap means resync from the REST
  snapshot `GET /api/v3/order_book?aggregate=false`, which lists every order with
  its `oid` and a `sequence` (https://docs.bitso.com/bitso-api/docs/general,
  https://docs.bitso.com/bitso-api/docs/list-order-book). No credentials in the
  subscribe. Contradiction: the channel page says `t` 0 = buy, the 2023-03-02
  changelog says the values were reversed
  (https://docs.bitso.com/bitso-api/docs/history-of-changes).
- **Bitfinex raw books (`prec: "R0"`)**: `[ORDER_ID, PRICE, AMOUNT]`, price 0
  deletes; `len` at most 250, so only a window of the book
  (https://docs.bitfinex.com/reference/ws-public-raw-books). Optional per-message
  sequence (`SEQ_ALL`) and top-25 checksum (`OB_CHECKSUM`); 30 public
  subscriptions per connection (https://docs.bitfinex.com/docs/ws-general).
- **Bitstamp `live_orders`**: described as an L3 order event stream with no
  snapshot by Tardis (third party, https://docs.tardis.dev/historical-data-details/bitstamp);
  official WS docs were behind a bot wall. REST `order_book?group=2` returns orders
  with ids, no auth (https://www.bitstamp.net/api/). Sequencing unverified.
- **Coinbase Exchange `full`/`level3`**: genuine L3 with sequence numbers
  (https://docs.cdp.coinbase.com/exchange/websocket-feed/channels), but
  authenticated since 2023-08-01 and aimed at institutions
  (https://x.com/CoinbaseExch/status/1684572420152381443,
  https://exchange.coinbase.com/). Advanced Trade is `level2` only.
- **KuCoin level3**: deprecated in 2022
  (https://kucoin.com/news/en-deprecate-the-kucoin-spot-futures-api).
- **Hyperliquid "L4"**: order-level with wallet addresses, only from a self-run
  non-validator node at about 100 GB/day
  (https://github.com/hyperliquid-dex/order_book_server).
- Binance, OKX, Bybit, Gemini: L2 only as far as known; not re-checked.

## Ranking for a second L3 exchange

1. **Bitso** `diff-orders` plus the REST snapshot: public, genuine order ids, and a
   different wire shape from Kraken (explicit +1 sequence, gap detection, resync
   from a REST snapshot rather than snapshot-on-subscribe and a checksum).
2. **Bitfinex `R0`**: public, order-level, optional sequence and checksum, but a
   250-order window.
3. **Bitstamp `live_orders`**: EU-based, order ids, sequencing unconfirmed.

Next step: a probe (`experiments/bitso_l3_probe.py`) to confirm Bitso is live and
reachable and settle the side encoding.

## Bitso probe results (2026-09-26)

`experiments/bitso_l3_probe.py btc_mxn 60`, run from the dev machine:

- **Live and reachable.** Subscribe answered `{"action":"subscribe","response":"ok"}`;
  keepalives arrive as `{"type":"ka"}`.
- **Sequenced and gapless**: 372 `diff-orders` messages, `sequence` +1 each time,
  zero gaps. Message keys: `book`, `payload`, `sent`, `sequence`, `type`. Almost
  every message carries one entry (one carried two).
- **Entry keys** `a` (amount), `d` (order creation time, ms), `o` (order id), `r`
  (price), `s` (status), `t` (side), `v` (value = price x amount) and `z`
  (undocumented; looks like an update time in ms, 0 on some cancels). A
  `completed` entry carries no `a` or `v`.
- **Side, settled empirically**: order ids seen both in the stream and in the
  snapshot give `t=0` = bid, `t=1` = ask (81 matches, no contradictions). That
  agrees with the channel page, not the "reversed" changelog note.
- **Statuses**: 189 `open`, 183 `cancelled`, 1 `completed` in a minute.
- **A price amend keeps the order id**: the same `o` appears `cancelled` at one
  price and `open` at another, with the same creation time `d`. 148 of 177 order
  ids appeared more than once. An L3 book for Bitso has to treat "open for a
  known id" as a replace (Kraken has an explicit `modify`).
- **Snapshot**: `GET /api/v3/order_book?aggregate=false` returned 3,497 bids and
  4,225 asks, each with `oid`, `price`, `amount`, `book`, and a `sequence` a few
  messages ahead of the first buffered one, so the documented
  buffer-then-splice resync works: the whole book, not a depth window.
