---
aliases: [order book, ob, l1, l2, l3, golden book, book adapter, journal replay, journal_replay, journal_slice]
sources: [src/order_book/**, src/book_adapter/**]
decisions: [decisions/0006-order-book-policies.md, decisions/0013-golden-reference-book.md, decisions/0011-desync-recovery.md, decisions/0008-book-adapter-layering.md, decisions/0010-connection-thread-handoff.md]
---

# Order book

The golden-reference order book: a deliberately simple, `std::map`-based
implementation of L1, L2 and L3 books that later fast, exchange-specific books
get validated against, plus the adapter that feeds it live captures and journals.
The decisions behind it: compile-time policies (`decisions/0006`), the golden
book as oracle (`decisions/0013`), desync and recovery (`decisions/0011`) and the
adapter's layering (`decisions/0008`). This is what is where, how it behaves and
how to use it.

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
| `UnsequencedL2Policy` | Deribit FIX market data, normalized to the same `L2Update`s | no `change_id`, so no gap check; unknown-level and crossed-book checks kept; own snapshot type (see "L2 without a `change_id`") |
| `L3Policy` | generic add/modify/delete by `order_id` | exchange-agnostic; optional `max_depth` |
| `KrakenL3Policy` | Kraken `level3` | wraps `L3Policy`; adds checksum verification and Add ordering by timestamp; `subscribed_depth` has no default |

`kraken_l3_wire.h` parses Kraken `level3` JSON into `KrakenL3Update`s;
`InstrumentScale` converts exchange decimals to `Price`/`Quantity` ticks and
lots. Both the scale (price/quantity decimals) and the Kraken depth are
per-instrument, per-subscription settings the caller supplies; nothing in the
library hardcodes them.

## How the books behave

### Engine and policy contract

The engine owns a reference to the listener, readiness, the `Apply`/
`ApplySnapshot`/`ApplyBatch` entry points and the matching seam. A granularity
policy applies updates and returns a policy-defined `ChangeSet`; the engine
detects each change-set shape with `if constexpr (requires ...)` and turns it into
listener calls. It never branches on which concrete policy it holds.

### Listener hooks

`OnTopOfBookChanged` is required by every granularity's listener concept, so a
missing or misspelled one is a compile error. The rest are optional, called only
if present: `OnLevelChanged`/`OnLevelDeleted` and `OnIntegrityCheckFailed` for
L2 and L3, and `OnOrderAdded`/`OnOrderModified`/`OnOrderDeleted` for L3.

### Message/batch boundary

`ApplyBatch(span<const Update>, meta...)` validates sequencing (`change_id`) or
the checksum once per exchange message, since one message carries several updates
under one `change_id` or checksum. Top-of-book notifications coalesce to once per
batch; order and level events still fire per update. Applying one update at a time
would make the checksum uncomputable mid-message and show listeners top-of-book
states that never existed on the exchange.

### Readiness and errors

`Uninitialized -> Ready -> Desynced`. `ApplySnapshot` clears and repopulates the
book and makes it `Ready`; `Apply`/`ApplyBatch` on a book that is not `Ready` is
a defined no-op, never undefined behavior. Errors come in three tiers:

1. **Feed-integrity anomalies** (`Gap`, `ChecksumMismatch`, `CrossedBook`,
   `UnknownOrder`, `UnknownLevel`) go to `OnIntegrityCheckFailed` and the book
   becomes `Desynced` (`decisions/0011`).
2. **API misuse** is an `assert()`, active in Debug and tests. Partial so far:
   `L2Levels` asserts on a `New` for a price already tracked, and `L3Policy`
   asserts an order it removes still has a level. Negative quantities are not
   guarded yet. A duplicate L3 add for a live `order_id` is not misuse: it is a
   defined remove-then-re-add (see "Kraken L3" below).
3. **Nothing is silently ignored**: a `Change` for an untracked L2 price reports
   `UnknownLevel`.

`std::expected` as a second result channel from `Apply` was considered and not
used, to keep one reporting channel; worth revisiting if a pull-based
differential harness is built.

### Query surface and `BookView`

All granularities expose `IsReady()` and `Best(Side)`. L3 adds `Levels(Side)`
(per-level order ids, for tooling) and `OrderQuantity(OrderId)`; there is no
general depth or full-order query. `BookView` is a granularity-agnostic,
comparable dump of a book: what the Kraken checksum and the replay tests read,
never a policy's storage (`decisions/0013`).

### Threads and ownership

Single-threaded and caller-synchronized. Listeners are called inline from the
apply call and must not re-enter it. The engine holds the listener by reference,
so it is neither assignable nor movable, and resetting a book means erasing and
recreating it.

### Wire encoding vs. stored state

`L1Update` keeps the wire's zero-quantity-means-no-level encoding, because that
is what an L1 feed sends. `L2Update` carries an explicit `L2Operation`
(new/change/delete), as Deribit sends it; inferring delete from a zero quantity
happened to work on one capture but relied on an unstated invariant. Whatever
the wire encoding, stored state and queries use `std::optional`, never a
sentinel.

### Level container

Levels are `LevelMap<K, V, Cmp>` (`types.h`), an alias for `std::map` with
`std::greater<>` for bids and `std::less<>` for asks, so a faster container is
a typedef change.

### L2 without a `change_id`

Deribit FIX market data has no `change_id` (`exchanges/deribit.md`), so it has
its own policy:

- **Shape.** The levels and per-update rules (unknown level, crossed book, the
  duplicate-`New` assert) live in `L2Levels`. `L2Policy` is `L2Levels` plus the
  `change_id` baseline and gap check; `UnsequencedL2Policy` is `L2Levels` alone,
  with its own `UnsequencedL2Snapshot`. Same `L2Update`, `L2ChangeSet` and
  engine.
- **Two types, not an overload.** An `ApplyBatch(updates)` overload on
  `L2Policy` would let a forgotten `ChangeIdMeta` compile and silently skip the
  one protection the sequenced book has. The engine's entry points are
  constrained on the policy accepting exactly the arguments given, and the tests
  `static_assert` the rejected combinations.
- **No synthesized `change_id`.** A counter made up by the adapter would satisfy
  the signature, but the gap check would compare it with itself and never fire.
- **FIX gaps are the session's job**: a `MsgSeqNum` jump reconnects
  (`decisions/0011`). The book still checks what it can without a sequence.

### Kraken L3: checksum, depth and add ordering

The generic `L3Policy` knows no exchange: add/modify/delete by `order_id`,
orders in arrival order per level, crossed-book and unknown-order detection, an
optional `max_depth`. `KrakenL3Policy` composes it and adds three things
(wire facts and how they were found: `exchanges/kraken.md`):

1. **Checksum verification.** `kraken_checksum.h` computes it over `BookView`,
   and `KrakenL3Policy` verifies it on every batch *and* on the snapshot
   (`ApplySnapshot` takes a `ChecksumMeta` too). Neither the engine nor
   `L3Policy` knows about it; a mismatch is the generic `ChecksumMismatch`. A
   listener could not do this check: it has no batch boundary and cannot desync
   the book.
2. **Depth truncation.** Kraken never deletes a level that falls out of the
   subscribed depth, so both sides are truncated to it after every batch and
   snapshot. The depth is the subscription's and has no default. Without
   truncation, an order re-entering the window arrives as a fresh `Add` with the
   same `order_id` and duplicates a stale copy. The checksum's own window is
   always 10 levels (`kKrakenChecksumLevels`), whatever the depth.
3. **Add ordering by timestamp.** Orders entering the window in one message are
   not listed in arrival order. `ApplyBatch` sorts only the `Added` events by
   their own `timestamp` and puts them back in the positions Adds held; other
   events keep their positions, because an `Added` can be followed in the same
   message by a `Modified`/`Deleted` of that order.

### Matching

`MatchingPolicyConcept` only requires `MatchingEnabled()`; `NoMatchingPolicy`
applies diffs as given ("trust the feed"). A real matching policy needs an order
submission entry point and fill notifications the concepts do not model yet
(`docs/questions/2026-09-23-matching-seam.md`).

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
  leaves the book desynced until the next connect or a fresh snapshot
  (`decisions/0011`). Nothing throws out of the adapter. `updates_before_snapshot`
  and `updates_while_desynced` are updates that were received but not applied.
- What desyncs: a drop desyncs every book of that connection, and so does an
  unparsable Kraken payload (its symbol is unknown, and it may have carried
  updates the books now miss). A Deribit FIX parse error that got as far as the
  symbol desyncs only that book; without one, all of them. An exception out of a
  book desyncs that book.
- A disconnect keeps the books rather than erasing them, so the last known state
  can still be inspected; erasing buys nothing, since a connect erases and
  recreates them anyway.
- Errors are logged for the first few of each kind and only counted after that,
  and only the first drop per connect is logged, so a broken stream is not a
  stream of log lines.
- Order ids are `HashOrderId`, a 64-bit hash, so two orders could in principle be
  conflated (odds estimated around 1e-9 at this scale, not measured). Interning
  them into dense ids needs an eviction scheme to stay bounded on a long run
  (`docs/questions/2026-09-23-order-id-interning.md`).

### Live: `BookService`

- One `RingSink` per connection copies each frame (its one payload copy) and
  pushes it without blocking. The journal has its own ring and its own copy;
  sharing one ring with two read cursors would copy once, but the ring library
  does not offer it (`docs/questions/2026-09-23-ring-copies-and-slots.md`).
- One book thread polls every connection's ring round-robin, at most 64 events
  per ring per pass so one backlog cannot starve another connection. When the
  rings are empty it spins, then yields, then sleeps 100 microseconds; it never
  blocks on a lock or condition variable, so `Stop()` is a flag and a join and
  there is no wakeup to lose.
- **Each ring entry carries the ring's drop count at the moment it was pushed**,
  and the consumer reports drops to the adapter before applying that entry.
  Reading the live counter after a pop would be wrong: the producer can be a
  whole ring ahead, so a drop after a connect could be seen before the connect
  is popped, and the connect's reset would wipe the desync. `Stop()` reports
  drops no later entry carried.
- Whatever escapes the adapter is caught, logged, counted (`InternalErrors()`)
  and the event skipped.
- Shutdown: every connection is stopped and joined, then `BookService::Stop()`
  drains every ring and joins the thread, then the summaries are logged. The
  books are declared before the capture set in `main()` because the sessions hold
  pointers to their sinks.
- Per-connection order is FIFO by construction; that `capture_sequence` strictly
  increases per connection is asserted by a test at the ring consumer, not
  checked at runtime.
- The Kraken fixture replays through the adapter with the same counts as the
  order-book replay test (1 snapshot, 6361 updates, no issues).

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
  handshake into the output. It exists only to exercise the
  `change_id`-sequenced `L2Policy`; nothing captures this channel live.
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
