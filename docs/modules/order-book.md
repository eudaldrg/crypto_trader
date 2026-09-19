---
aliases: [order book, ob, l1, l2, l3, golden book]
sources: [src/order_book/**]
decisions: [decisions/0006-order-book-design.md]
---

# Order book

The golden-reference order book: a deliberately simple, `std::map`-based
implementation of L1, L2 and L3 books that later fast, exchange-specific books
get validated against. Not yet wired to the feed handler's journal or
`MessageSink` output. Design rationale is in `decisions/0006`; this is what is
where and how to use it.

## Shape

`OrderBook<GranularityPolicy, ListenerT, MatchingPolicy>` (`engine.h`) owns the
readiness state machine and turns a policy's `ChangeSet` into listener calls.
The policy is passed to the constructor by value, so one with settings is
built configured. Everything is resolved with concepts (`concepts.h`), no
virtual dispatch.

| Policy | Wire shape | Notes |
|---|---|---|
| `L1Policy` | best price/quantity per side | |
| `L2Policy` | Deribit `book`: `[new/change/delete, price, qty]` per level | `change_id`/`prev_change_id` gap detection |
| `L3Policy` | generic add/modify/delete by `order_id` | exchange-agnostic; optional `max_depth` |
| `KrakenL3Policy` | Kraken `level3` | wraps `L3Policy`; adds checksum verification and Add ordering by timestamp; `subscribed_depth` has no default |

`kraken_l3_wire.h` parses Kraken `level3` JSON into `KrakenL3Update`s;
`InstrumentScale` converts exchange decimals to `Price`/`Quantity` ticks and
lots. Both the scale (price/quantity decimals) and the Kraken depth are
per-instrument, per-subscription settings the caller supplies; nothing in the
library hardcodes them.

## Fixtures and tools

The replay tests run against two real captured sessions, stored compressed in
`src/order_book/tests/data/capture_fixtures.tar.xz` and unpacked into
`build/<preset>/order_book_test_data/` when CMake configures.

- Kraken `level3` BTC/USD (price decimals 1, quantity decimals 8, subscribed at
  Kraken's default depth 10): captured with the real `kraken_feed_handler`
  binary. The journal never contains the WebSocket token (it journals inbound
  messages only).
- Deribit `book.BTC-PERPETUAL.raw`: captured with
  `experiments/deribit_ws_book_probe.py`, which does not write its auth
  handshake into the output.

To replace a fixture, capture again, confirm the file holds no credentials
without printing its contents, update the exact message counts asserted in the
replay tests, and repack with
`tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=... -cJf capture_fixtures.tar.xz <files>`.

`kraken_journal_dump` replays a Kraken journal through the real `KrakenL3Policy`
and writes one JSON frame per message; `kraken_journal_viewer.py` steps through
the frames (F5 / Shift+F5) so the real book's behavior can be watched. The
viewer contains no book logic. The dump tool takes `--price-decimals`,
`--quantity-decimals` and `--depth`; its defaults match the checked-in capture.
