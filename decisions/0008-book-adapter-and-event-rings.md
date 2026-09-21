# 0008. Book adapter and event rings: wiring the golden books to captures

## Context

`decisions/0006` built the golden order books and validated them against short
real fixtures, but nothing fed them a live connection or a long capture, and the
feed handler (`decisions/0004`) only journaled. Issue #14 connects them: a
component that turns a connection's frames into per-symbol books, a live path
that carries frames to it without touching the connection thread, and a replay
driver that runs a captured journal through the same code so the book can be
profiled on real data. The discussion that produced the shape is in
`docs/investigations/2026-09-21-issue-14-architecture.md`; this records what was
built and why. Where the code and that note differ, this is the code.

The journal thread, `connect_id` and the disconnect notification this depends on
are in `decisions/0004` ("Journal thread and connect_id").

## Decision

### Where the adapter lives: `src/book_adapter/`, a library of its own

`book_adapter` depends on `order_book` and on `feed_handler`. The dependency arrows
that already existed stay as they were: `order_book` knows nothing of
`feed_handler` (`decisions/0006`), and `feed_handler` knows nothing of
`book_adapter`.

- Inside `order_book` was rejected: it would gain a `feed_handler` dependency, the
  one `decisions/0006` keeps out. That is also why the Deribit FIX normalizer
  (`deribit_fix_book_wire.h`) is here and not next to `kraken_l3_wire.h`: it reads
  `feed_handler`'s FIX parser.
- Inside `feed_handler` was rejected: the capture library would depend on the book
  code, and a capture that only journals would link it.

The live wiring is therefore not in `feed_handler` either. The capture library
offers two generic hooks, `CaptureConnection::AddSink` and the `BeforeStart`
callback of `CaptureSet::StartAll`, and the capture binary (`feed_handler_capture`,
the one target that links both libraries) plugs the books into them through
`book_adapter/book_wiring`. The callback exists because `AddSink` is only legal
before `Start()`, while a Kraken scale exists only after the `AssetPairs` lookup,
which `StartAll` runs after the Deribit connections have started. It is called
once per connection, just before that connection starts, with the config entry and,
for Kraken, the `RestClient` whose cache the lookup filled.

### One adapter, driven inline by replay and behind a ring live

`BookAdapter` has no threads, rings or locks, so the same object serves both
drivers and behaves identically in each.

- **Replay** (`JournalReplay`, the `journal_replay` tool) calls the adapter
  directly from the journal reader: no ring, no thread. A replay cannot lose
  frames, and a ring in front of it would add a thread for nothing. It reads an
  ordered list of one exchange's journals, reports parse and apply wall time
  separately from the reading, and is the profiling harness the per-frame copy in
  `decisions/0004` is to be measured with. Exit codes: 0 complete, 1 usage error or
  a journal that cannot be replayed at all, 2 a journal that stopped early (the
  report is printed, but its counts are not the whole capture).
- **Live** (`BookService`): one `RingSink` per connection (a `MessageSink` that
  copies each frame, the one payload copy, and pushes it without blocking) and
  one book thread that polls every connection's ring round-robin, up to 64 events
  per ring per pass so a backlog cannot starve another connection. Parsing
  happens on the book thread, never on a connection thread. The thread spins, then
  yields, then sleeps 100 microseconds when its rings are empty, and never blocks
  on a lock or a condition variable; `Stop()` is a flag and a join, so there is no
  wakeup to lose. Rings hold 65,536 events by default (`BookService::Config`, no
  config key yet).
- **Two rings per connection, not one with two cursors.** The journal and the
  books each have their own ring and their own copy of a frame. One ring with two
  read cursors would copy once, but moodycamel does not offer it; revisit when
  profiling asks.
- **Shutdown order** is: stop and join every connection (`Run`), then
  `BookService::Stop()` drains every ring and joins the thread, then the summary
  lines are logged. The books are declared before the capture set in `main()`
  because the sessions hold pointers to their sinks.
- **The book thread starts after `StartAll` returns**, since every connection must
  have its ring first. Deribit connections start before the `AssetPairs` lookup
  (up to about 30 s), so their rings buffer for that long; at Deribit's measured
  rate that is a few hundred events, far below the ring size.

Replay treats a file boundary as no event: only a connect record ends the previous
connect (`OnDisconnect`), and the end of the last file does. A rotated continuation
file (#5) carries no connect record and must not stale a book.

### The adapter dispatches on the connection's configured source

`BookSettings::source` says what a connection's payloads are, and the adapter
trusts it rather than `CaptureFrame::source`. A journal does not record the source
per record (`decisions/0004`), so a replayed frame carries `kUnknown`. Live and
replay therefore go through the same path, and the source is given once per
connection: from the config live, from the journal header's exchange tag in
replay. A frame for a source the adapter has no book for is counted, not applied.

### Books: lazy, per symbol, reset by rebuilding

One book per (connection, symbol), created on that symbol's first snapshot. The
engine has no `Reset()` and holds its listener by reference, so it is neither
assignable nor movable: a reset is erase and recreate, and a book the adapter
cannot trust is flagged in the adapter's own wrapper (`KrakenBook`, `DeribitBook`)
and reads as `Desynced` until its next snapshot. Kraken books are
`KrakenL3Policy` (checksum verified on every message, `decisions/0006`); Deribit
FIX books are `UnsequencedL2Policy` (see "Unsequenced L2" below).

Counting is per parser entry, not per frame: a Kraken message whose `data[]` holds
two symbols is two snapshots or updates. `ParseKrakenL3Message`, which read only
`data[0]` and dropped the `symbol`, was removed in favour of
`ParseKrakenL3Messages`, which returns one entry per `data[]` element with its own
symbol and checksum. For Deribit FIX each `35=W` is a snapshot and each `35=X` one
update, whatever the number of entries it carries.

### Stale semantics: a disconnect keeps the books, a connect rebuilds them

- **Disconnect:** the connection is flagged stale and every frame until the next
  connect is ignored and counted (`frames_ignored_stale`). The books are kept, so
  the last known state can be inspected; they are not marked desynced individually,
  the stale flag is what stops updates reaching them.
- **Connect:** the connection's books are erased and rebuilt from the snapshots
  that follow, and frames are accepted again.
- Erasing on disconnect was rejected: it loses the last-known state for
  observability and buys nothing, since a reset is erase and recreate either way.
- A journal rotation must not go stale, which is why replay does not disconnect on
  a file boundary (above).

### Drop and desync policy: never block, never crash, stay desynced

A book bug must not take capture down. Nothing in the adapter or the book thread
throws out or blocks; the book thread never latches fatal and cannot change the
exit code, and books are an add-on to a capture whose job is journaling.

- **A live ring that is full drops the frame.** It never blocks: a blocked
  connection thread stalls the socket, and Kraken drops slow consumers. The ring
  counts the drop. Connect and disconnect events use the ring's unbounded push and
  are never dropped, because losing a connect leaves a book built from two
  snapshots.
- **Each ring entry carries the ring's drop count at the moment it was pushed**,
  and the consumer reports drops to the adapter before applying that entry.
  Comparing the live counter after a pop was rejected: the producer can be a whole
  ring ahead of the consumer, so a drop after a connect could be seen before the
  connect is popped, and the connect's book reset would then wipe the desync.
  `BookService::Stop()` reports drops no later entry carried.
- **A drop desyncs every book of that connection.** So does an unparsable Kraken
  payload (which symbol it was for is unknown, and it may have carried updates the
  books now miss). A Deribit FIX parse error keeps the symbol when it got that far,
  and then only that book desyncs; without one, all do. An exception out of a book
  desyncs that book. An integrity issue (`decisions/0006`) is counted by kind and
  logged, and the engine desyncs the book.
- **Recovery is the next natural reconnect.** A desynced book stays desynced until
  its connection's next `connect_id` or a fresh snapshot for that symbol (Kraken
  sends one only on subscribe, so in practice the next connect). Nothing asks the
  socket to reconnect: a thread-safe reconnect request on both clients is real work
  with its own risks and is a follow-up, not part of this.
- Errors are logged for the first few of each kind and only counted after that, and
  only the first drop notification per connect is logged, so a broken stream is not
  a stream of log lines. `BookService` also catches whatever escapes the adapter,
  logs and counts it (`InternalErrors()`), and skips the event.

### Unsequenced L2 for Deribit FIX

Deribit's FIX market data carries no `change_id`, so its books use
`UnsequencedL2Policy`, with the parsed `35=W` as an `UnsequencedL2Snapshot` and each
`35=X` as one batch. Why it is a separate policy and not an overload or a
synthesized `change_id` is in `decisions/0006` ("L2 without a `change_id`").

### Order-id interning is deferred

Kraken order ids are mapped with `HashOrderId`, a 64-bit hash. Interning them into
dense ids would remove any chance of two orders being conflated, but without an
eviction scheme the table grows without bound on a long live run, and the collision
odds at this scale are small (the plan estimated about 1e-9; not measured).
Deferred until it is needed, together with the fast books that would want dense ids.

### Live enablement: `order_books`, default off

The top-level config key `order_books` (default `false`) turns the live books on. A
capture tool's job is journaling, and a new component should not change that until
it has run for a while. With it off no connection is given a sink, no thread is
started and no book code runs. The keys that feed it are in
`docs/modules/feed-handler.md`.

- **Kraken depth** is a connection key (`10` default, `100` or `1000`), always sent
  explicitly on the subscribe, because the depth a book is built for and the depth
  subscribed must be the same number or the checksum desyncs silently. The three
  values come from Kraken's documentation (`exchanges/kraken.md`); only `10` has
  been observed live.
- **Kraken scale** comes from the `AssetPairs` lookup that already runs: the finest
  decimals among the connection's symbols, since one scale serves the whole
  connection. A symbol missing from the lookup (or a failed lookup) is not a guess
  from the others, a wrong scale would desync every checksum without failing loudly;
  it is logged and the connection is captured without books.
- **Deribit scale** has no lookup, so `price_decimals` and `quantity_decimals` are
  connection keys applied to every symbol on it. With `order_books` on, a Deribit
  connection missing either is refused at startup (`CheckLiveBookConfig`, exit code
  2). Per-symbol scale was rejected: more schema for a case that does not exist at
  5 to 10 symbols.
- The two failures differ on purpose. A missing Deribit key is known from the
  config before anything starts, so a capture asked to run books it cannot build is
  refused instead of quietly running without them. A missing Kraken symbol is known
  only after a network lookup, and books are never a reason to fail a capture that
  otherwise works.

### Evidence

- The Kraken fixture replays through the adapter with the same counts as the
  order-book replay test (1 snapshot, 6361 updates, no issues).
- A real Deribit FIX slice (the first snapshot and 800 incrementals of a capture)
  is committed in `src/order_book/tests/data/capture_fixtures.tar.xz` and replayed
  through the adapter. It exists because synthetic FIX written from the docs would
  not catch a wrong assumption about the wire, which is what a real Deribit
  session caught in `decisions/0006`. The real wire shape agreed with the
  normalizer. The `journal_slice` tool cut it: it drops every `35=A` Logon
  (Deribit's inbound Logon carries `RawData(96)` and `Password(554)`), refuses to
  write a slice that still holds one of `35=A`, `96=` or `554=`, and prints only
  counts. The test's expected `35=W` and `35=X` counts come from that tool's raw
  byte scan, not from the adapter.
- Live-path tests cover ring overflow, desync and recovery, and concurrent
  producers against the book thread. Every test runs under a memory cap
  (`docs/tasks/testing.md`).

## Consequences

- The book code can be profiled on a long capture with `journal_replay` before any
  live run, and the live path is the same adapter behind a ring.
- The books are readable only while the book thread is not running (before `Start()`
  or after `Stop()`). There is no cross-thread read path yet, so nothing can query
  a live book; that arrives with the strategy interface.
- Per-connection ordering is by construction (one producer per ring, one consumer,
  FIFO). The `capture_sequence` of a session's frames being strictly increasing per
  connection and per `connect_id` is asserted by a test that audits it at the ring
  consumer. Neither the production consumer nor the adapter keeps a per-frame
  record of it, so there is no runtime check.
- A single book thread serves every connection, so one slow connection's book
  work delays the others' books (never their journaling).
- `clang-tidy` prints warnings that the commit-time hook does not fail on, because
  `.clang-tidy` has an empty `WarningsAsErrors` (`decisions/0005`). Whether to
  tighten that is the owner's call, listed below.

## Follow-ups

Not done, and not decided:

- A config key for the book ring size (and for the journal ring size, which also
  has none).
- Order-id interning, with an eviction scheme, alongside the fast books.
- Sharding the book thread per symbol or per connection group, once one thread is
  the bottleneck.
- Strategy-facing queries on the books, and the ring that carries normalized book
  deltas out to strategies (a different ring from the raw-frame ones here).
- A thread-safe reconnect request for a desynced book, on both clients.
- Preallocated ring slots instead of a `std::vector` per frame, and one ring with two
  cursors instead of two copies, both when profiling asks.
- A runtime check of `capture_sequence` at the ring consumer, if the test-only audit
  is not enough.
- Kraken depths `100` and `1000` have not been run live.
- Tightening `.clang-tidy` `WarningsAsErrors` so the commit-time hook fails on
  warnings: an owner decision, not made here.
