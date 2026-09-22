---
aliases: [order book, ob, l1, l2, l3, golden book, book adapter, journal replay, journal_replay, journal_slice]
sources: [src/order_book/**, src/book_adapter/**]
decisions: [decisions/0006-order-book-design.md, decisions/0008-book-adapter-and-event-rings.md]
---

# Order book

The golden-reference order book: a deliberately simple, `std::map`-based
implementation of L1, L2 and L3 books that later fast, exchange-specific books
get validated against, plus the adapter that feeds it live captures and journals.
Design rationale is in `decisions/0006` (the books) and `decisions/0008` (the
adapter); this is what is where and how to use it.

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
| `UnsequencedL2Policy` | Deribit FIX market data, normalized to the same `L2Update`s | no `change_id`, so no gap check; unknown-level and crossed-book checks kept; own snapshot type (`decisions/0006`) |
| `L3Policy` | generic add/modify/delete by `order_id` | exchange-agnostic; optional `max_depth` |
| `KrakenL3Policy` | Kraken `level3` | wraps `L3Policy`; adds checksum verification and Add ordering by timestamp; `subscribed_depth` has no default |

`kraken_l3_wire.h` parses Kraken `level3` JSON into `KrakenL3Update`s;
`InstrumentScale` converts exchange decimals to `Price`/`Quantity` ticks and
lots. Both the scale (price/quantity decimals) and the Kraken depth are
per-instrument, per-subscription settings the caller supplies; nothing in the
library hardcodes them.

## Wired to captures: `src/book_adapter/`

`BookAdapter` turns a connection's frames, connects and disconnects into one book
per (connection, symbol): `KrakenL3Policy` books for Kraken `level3`,
`UnsequencedL2Policy` books for Deribit FIX market data. It is single-threaded and
has no ring; two things drive it.

- **Live** (`order_books = true`, `docs/modules/feed-handler.md`): one book thread
  behind a ring per connection, `BookService` in `book_service.h`. Books are only
  inspectable after it stops.
- **Replay** (`journal_replay`): the same adapter called inline from the journal
  reader, no ring.

What it does, in the terms a reader of a log or a report needs:

- A book is created on its symbol's first snapshot. A connect erases the
  connection's books; a disconnect keeps them but marks the connection stale and
  ignores frames until the next connect. A journal file boundary alone is not a
  disconnect (a rotated continuation must not stale a book); a connect record is.
- It dispatches on the connection's configured source, not on `CaptureFrame::source`:
  a replayed journal frame carries `kUnknown`, because the journal does not record
  the source per record.
- Kraken is parsed per `data[]` entry (`ParseKrakenL3Messages`), so a message with
  two symbols is two snapshots or updates in the counts. Deribit FIX counts one
  snapshot per `35=W` and one update per `35=X`. `deribit_fix_book_wire.h` reads
  the FIX message with the project's own parser; a group that ends short fails the
  whole message so a book never applies half of one.
- A dropped frame, a parse error, an apply exception or an integrity issue (gap,
  checksum mismatch, crossed book, unknown order or level) is counted, logged and
  leaves the book desynced until the next connect or a fresh snapshot. Nothing
  throws out of the adapter. `updates_before_snapshot` and `updates_while_desynced`
  are updates that were received but not applied.
- Order ids are `HashOrderId`, a 64-bit hash; interning is deferred
  (`decisions/0008`).

### `journal_replay`

```
journal_replay [--price-decimals N] [--quantity-decimals N] [--depth N] [--no-timing] <journal> [<journal> ...]
```

Replays one connection's journals, in the order given, through the real adapter and
prints the counts, integrity issues by kind, and parse time and apply time
separately from the time spent reading the files. It is the profiling harness for the
book code: run it on a long capture, not the fixtures. All journals must be one
exchange (the header tag picks Kraken or Deribit). What a journal cannot say for
itself is a flag: the scale and, for Kraken, the depth; the defaults are the checked-in
Kraken capture's (BTC/USD, price decimals 1, quantity decimals 8, depth 10), so a
Deribit journal needs its own (BTC-PERPETUAL is 1 and 0).

Exit codes: 0 complete, 1 usage error or a journal that cannot be replayed (an
unreadable file, an exchange with no book, mixed exchanges), 2 a journal stopped
early (a torn tail or corruption): the report is still printed, but its counts are
not the whole capture.

## Fixtures and tools

The replay tests (`src/order_book/tests/` and `src/book_adapter/tests/`) run against
three real captured sessions, stored compressed in
`src/order_book/tests/data/capture_fixtures.tar.xz` and unpacked into
`build/<preset>/order_book_test_data/` when CMake configures.

- Kraken `level3` BTC/USD (price decimals 1, quantity decimals 8, subscribed at
  Kraken's default depth 10): captured with the real `kraken_feed_handler`
  binary. The journal never contains the WebSocket token (it journals inbound
  messages only).
- Deribit `book.BTC-PERPETUAL.raw`: captured with
  `experiments/deribit_ws_book_probe.py`, which does not write its auth
  handshake into the output.
- Deribit FIX `BTC-PERPETUAL` (`deribit_fix_capture.journal`, 266 KB): the first
  snapshot and 800 incrementals of a real `deribit_feed_handler` capture, cut
  with `journal_slice` and replayed by `deribit_fix_replay_test.cpp` in
  `src/book_adapter/tests/` (price decimals 1, quantity decimals 0). Unlike the
  other two, this journal does hold a credential-shaped record: Deribit's
  inbound Logon (`35=A`) carries `RawData(96)` and `Password(554)`.
  `journal_slice` drops it, and the test asserts the slice has no `35=A`, `96=`
  or `554=`.

To replace a fixture, capture again, confirm the file holds no credentials
without printing its contents, update the exact message counts asserted in the
replay tests, and repack with
`tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=... -cJf capture_fixtures.tar.xz <files>`.

To cut a new FIX slice from a full capture, run
`journal_slice [--max-incrementals N] <input> <output>`. It copies the journal
byte for byte up to the Nth `35=X`, skips every `35=A`, refuses to write a slice
that still holds a Logon, `96=` or `554=`, and prints only counts, including the
raw `35=W` and `35=X` counts the replay test asserts. Never print the input or
the slice; check them by counting matches (`grep -a -o -P '\x0196=' f | wc -l`).

`kraken_journal_dump` replays a Kraken journal through the real `KrakenL3Policy`
and writes one JSON frame per message. It follows one symbol, the first it sees in
the journal, and warns about messages for others, so on a multi-symbol capture it
shows only that one; `journal_replay` covers every symbol. `kraken_journal_viewer.py`
steps through the frames (F5 / Shift+F5) so the real book's behavior can be
watched. The viewer contains no book logic. The dump tool takes `--price-decimals`,
`--quantity-decimals` and `--depth`; its defaults match the checked-in capture.
