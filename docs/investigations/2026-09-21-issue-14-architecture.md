# Issue #14: wiring the order book to captured data

Investigated 2026-09-21. Question: does the proposed architecture make sense
(one OB thread fed by SPSC queues, journal parallel to the book, Live/Replay
as different producers)? Read-only; nothing was run.

## Verdict

Yes. It is largely the end state `decisions/0004` already describes. Three
corrections came out of the discussion and are folded in below: the journal is
written on its own thread (not synchronously on the connection thread), the
"incarnation" concept is renamed `connect_id`, and a disconnect notification
is added so a book goes stale as soon as its feed drops.

## What the code does today

- `CaptureSession::OnWireMessage` (`src/feed_handler/capture_session.cpp`)
  stamps a frame once, calls `writer_->OnFrame(frame)`, then every registered
  sink, all on the connection's own thread. The journal writer is a concrete
  member, not a sink.
- The journal is "upstream" of the book only in call order and failure
  isolation. Sinks get the same in-memory `CaptureFrame`; nothing is read back
  from the journal, so the live path has no dependency on the journal format
  (#5) or the k-way merge (#6). Only replay touches `JournalReader`.
- `CaptureFrame::payload` is a non-owning span valid only inside `OnFrame`
  (`message_sink.h`, ADR 0004). Enqueueing needs a copy at the push site.
- The journal write is a buffered `ofstream` (1 MiB buffer, no per-record
  fsync). Most calls are a memcpy, but each buffer flush is a `write()`
  syscall, and with the writer first that syscall sits in front of every book
  update. ADR 0004's backpressure bullet calls this "v1 accepts this" and
  flags it for revisiting.
- `BeginIncarnation` (to be renamed, below) runs on every successful
  (re)connect: closes the old file, bumps a per-connection counter, opens
  `<prefix>-<n>-<UTC timestamp>.journal`, writes a marker record, then calls
  `OnIncarnation` on the sinks. The counter is per `CaptureSession`, i.e. per
  `[[connections]]` entry, and restarts at 1 each process run.
- Sinks are told nothing when a connection drops; the only notification is at
  the next successful connect.
- Reconnect: Kraken uses IXWebSocket auto-reconnect plus a watchdog
  `ForceReconnect` (private); Deribit reconnects on session-level gaps and peer
  logout. Neither exposes a thread-safe "please reconnect" to a book.

## The existing order book

- `OrderBook<Policy, ListenerT, MatchingPolicy>` (`engine.h`) owns readiness,
  `ApplySnapshot`, `Apply`, `ApplyBatch`. Single-threaded, caller
  synchronized (ADR 0006): one OB thread owning every book is what it assumes.
- `KrakenL3Policy(depth)` wraps `L3Policy` with checksum verification.
  `L2Policy` requires a `ChangeIdMeta` per batch.
- `ParseKrakenL3Message(json, scale, mapper)` parses with nlohmann.
- The engine has no `Reset()` and holds `listener_` by reference, so books are
  not assignable; a reset means erase and recreate.
- `HashOrderId` is `std::hash`; an adapter that must never conflate two
  orders should intern ids instead.
- The book library depends on nothing in `feed_handler`; only its tests link
  it, for `JournalReader`.

## Gaps that #14 must cover

- No adapter: dispatch on `FrameSource`, one book per (connection, symbol),
  created lazily on the first snapshot.
- No scale or depth in configuration. Kraken depth is implicit (default 10);
  Kraken scale could come from the `AssetPairs` lookup, Deribit scale from
  config until #9.
- No Deribit FIX to `L2Update` normalizer. `fix::ReadGroup` exists, nothing
  consumes it. FIX has no `change_id` / `prev_change_id`, so either synthesize
  one (the gap check becomes a tautology) or add a meta-less L2 path.
- The committed Deribit fixture (2069 changes, 0 gaps) is WS `book` JSON, not
  FIX, so the FIX path needs its own journal fixture and expected counts.
  Only the Kraken fixture fits the "same counts as the order-book replay
  tests" acceptance bullet as written.
- "Rely on reconnect-and-resnapshot after an `IntegrityIssue`" does not exist:
  no cross-thread reconnect request. Add one or scope to "log, count, stay
  desynced until the next natural reconnect".
- moodycamel is named in ADR 0002 but is not in `CMakeLists.txt`.

## Design that came out of the discussion

- The connection thread does no disk I/O. It stamps a frame once (capture
  sequence number and timestamp fixed at stamp time), copies the payload into
  two SPSC rings, one per consumer, and returns. About 100-200 bytes of extra
  memcpy is negligible against a syscall. A single ring with two read cursors
  would need one copy but is not offered by moodycamel; revisit later.
- One journal thread drains the journal rings and runs `JournalWriter`
  unchanged. One OB thread polls one ring per connection (N producers means
  N rings; sharding later means changing which rings a thread polls).
- Parsing happens on the OB thread, not the socket thread.
- Journal ring overflow is fatal to the capture (same category as a failed
  write in ADR 0004); size the ring for seconds of traffic. The fatal signal
  is asynchronous: the journal thread sets the existing fatal latch.
- Book ring overflow in live mode: never block, drop, count, mark the book
  desynced. In replay: block or run inline; replay cannot lose frames. Counters
  belong with #8.
- Control events travel in-band through the same rings so they stay ordered
  with frames: connect (new `connect_id`), disconnect (book goes stale
  immediately), plus the journal's marker record. Opening the journal file at
  connect can stay synchronous; it is rare and off the hot path.
- Two rings are distinct from #10's ring: these carry raw frame copies into
  the OB thread, #10's carries normalized book deltas out to strategies.
- Simulation is a different book (matching, order acceptance), not another
  producer into the raw-frame ring.

## Naming: `connect_id`

The old term "incarnation" means one established connection, from connect and
subscribe until it drops. It is not a journal rotation and not a day. It
bundled two ideas: a stream discontinuity (book must reset) and a file
boundary. They coincide today only because rotation is not implemented; #5
will separate them. A rotation must not stale a book.

Renamed to `connect_id` (`feed_handler_connect_id` in prose): a per-connection
counter that increments on each successful reconnect. It is not global; two
connections can both be on id 3, and a reconnect of one feed does not affect
the other. A book is identified by (connection, symbol) and reset when its
connection's `connect_id` changes. It gives no ordering across connections
(that is #6). About 209 mentions in about 25 files need renaming.

File layout stays file-per-`connect_id` for now. A single file per process
run with in-band markers, split only by rotation, is cleaner but changes the
header and the sequence numbering, so it belongs to #5 (format v2).

## Tension with #5

If resnapshot without reconnect is unsupported, rotation needs a book inside
the journal path to synthesize a snapshot, which would put the book upstream
of the journal. Note it on #5; it does not block #14.

## Minor

`FrameSource::kRakenJson` in `message_sink.h` looks like a typo for
`kKrakenJson`.
