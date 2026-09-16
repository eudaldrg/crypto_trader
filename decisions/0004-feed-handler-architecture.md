# 0004. Feed handler architecture: modes, journaling, threading seam

## Context

Before writing any feed-handler code, three usage modes need to be designed
for even though only one is being built now, because retrofitting the seams
between them later would touch the same code repeatedly:

1. **LiveTrading**: real exchange connections, subscription/gap handling,
   feeding parsed messages to order books strategies read from. These order
   books do not need matching; self-match prevention (if any) is a strategy
   concern, not the book's.
2. **Replay**: deterministically replays a captured day for debugging. If N
   exchanges fed M strategies live, every strategy must receive the exact
   same callback sequence in the exact same order on replay.
3. **Simulation**: a fake exchange replays captured messages in
   best-approximated real order, maintains *matching* order books, and
   accepts orders from an order-entry module hooked into strategies —
   the same hook that in LiveTrading sends real orders and in Replay is a
   no-op (or logs to a digest to confirm the strategy would have sent it).

The goal is for strategy-facing code to be identical across all three modes;
only the modules nearest the exchange (live socket vs. journal reader vs.
fake matching engine) differ. That only works if the interface between
"exchange-facing" and "everything else" is fixed now, before mode 1 is even
built.

Scope of this ADR's first implementation slice: a feed handler that connects
to **Kraken's `level3` WS** and journals raw messages to disk. No order book,
no Deribit yet — Deribit FIX is the second concrete backend, added once the
Kraken path proves the abstraction is actually generic and not
Kraken-shaped. Order books, strategies, and order entry are future ADRs.

## Decision

### Mode-agnostic seam: `MessageSink`

Every exchange connection (live socket, or a replay-journal reader standing
in for one) emits **capture frames** — raw wire bytes exactly as received,
plus capture metadata — into a `MessageSink` interface:

```
on_frame(capture_frame) -> void
```

**Ownership contract (this is the part the seam actually depends on):**
`capture_frame` is a non-owning view (pointer + length) into a buffer valid
only for the duration of the `on_frame` call — the caller may reuse or free
that memory the instant `on_frame` returns. Any sink that needs the data to
outlive the call (e.g., a future SPSC ring feeding a consumer on another
core) must copy it *itself*, on the producer side, before returning. This is
what makes the multithreading transition below a same-interface change: the
copy already has to happen somewhere the moment a second thread is involved,
so putting it at the ring's push site rather than inventing it later is free.
`std::span`-like framing also matters for the future simdjson step
(`decisions/0002`): simdjson's on-demand API requires `SIMDJSON_PADDING`
bytes of slack past the declared length, so the receive buffer should be
over-allocated by that much from the start rather than forcing a copy into a
`padded_string` later purely to satisfy the parser.

Dispatch mechanism (virtual `MessageSink` vs. a compile-time policy /
template parameter) is deliberately **not fixed by this ADR**. v1 has one
sink (the journal writer) and virtual dispatch costs nothing observable
there. Once the order book is a sink sitting in the per-message hot path,
this call is exactly the kind of thing the stated <100ns/insert goal would
be judged on — revisit with real numbers then, not speculatively now.

v1 has exactly one sink implementation: the journal writer. This is
deliberately the *only* seam that changes across modes and threading models:

- **LiveTrading** later adds a second sink (the order book).
- **Replay** swaps the live socket source for a journal-reading source that
  drives the same `on_frame` calls in the same order they were captured —
  strategy/book code cannot tell the difference. (What "same order" means
  precisely, and replay pacing, is intentionally left to a future
  Replay-mode ADR — see Consequences.)
- **Multithreading** (see below) changes `on_frame`'s implementation from "call
  directly" to "push a copy onto an SPSC ring," not the call site or the
  interface.

### Threading model: epoll-per-thread-group is the end-goal; v1 is IXWebSocket-constrained

**End-goal, still standing**: a thread owns an epoll instance over a *group*
of connection sockets it's responsible for — not necessarily one socket per
thread, and not necessarily all sockets on one thread either. This is the
concrete mechanism behind "give dedicated cores to specific parts of the
feed handler" — partition connections into groups (by exchange, by
symbol-set, whatever turns out to matter), hand each group's sockets to one
epoll-owning thread, pin that thread to a core. This wasn't dropped; it's
just not reachable for every connection in v1, for one specific reason:

- **IXWebSocket (`decisions/0002`) owns a background thread per `WebSocket`
  internally and doesn't expose the underlying fd** — there's nothing to
  hand to an external epoll set without bypassing the library's own
  reconnect/ping handling. So for as long as Kraken goes through
  IXWebSocket, that connection runs on IXWebSocket's own thread as a
  standalone exception to the epoll-group model, not a participant in it.
- **Deribit FIX is unaffected** — it's hand-rolled over a raw socket from
  day one (`decisions/0001`/`0002`), so it owns its fd outright and can join
  an epoll-group thread as soon as it exists; no library is in the way there.
- This is now a second, independent reason (alongside the protocol-level
  learning motivation already in `decisions/0002`) to eventually replace
  IXWebSocket with the hand-rolled RFC6455 client: doing so hands the fd
  back and lets Kraken connections join the same epoll-group model as
  everything else, instead of remaining a permanent special case.
- **v1 concretely**: one Kraken connection on IXWebSocket's own thread,
  calling `on_frame` synchronously into its own journal writer. No
  synchronization is needed yet regardless of grouping model, because each
  connection — grouped on a shared epoll thread or standalone on a
  library-owned one — writes to its own journal file; there is no shared
  mutable state between connections until the fan-in seam below is reached.
- The fan-in seam is unchanged by any of this: the day something needs input
  from more than one connection's thread (a merged sequencer, or an order
  book pinned to its own dedicated core reading from several feeds), that
  consumer gets fed via a moodycamel SPSC ring per producer thread
  (`decisions/0002`) — a push-from-many-threads / pull-from-one-consumer
  pattern layered on top, not a redesign of the epoll-group threads
  themselves.

### Journal format v1: raw bytes, not a unified schema

Per the standing decision to avoid premature unification, the journal stores
the exchange's own wire encoding verbatim — no re-encoding step, since
Kraken already arrives as JSON text and Deribit FIX already arrives as
tag=value bytes:

- **One append-only file per (exchange, connection-incarnation) — not per
  symbol.** A single Kraken WS connection interleaves every subscribed
  symbol on one TCP stream in true arrival order; splitting by symbol at
  capture time would mean parsing before journaling (contradicting
  raw-as-received) and would destroy the one true arrival order the capture
  sequence number below exists to preserve. Symbol filtering is a read-time
  index concern, not a file-layout concern.
- Each record: a capture timestamp (see below), an in-process monotonically
  increasing **capture sequence number**, and the length-prefixed raw bytes.
- **Inbound wire messages only are journaled** — including every
  control/heartbeat/status message, not just book-relevant ones (disk is
  cheap; selective journaling would violate the "don't lose information"
  principle). **Outbound requests are excluded from the raw journal**, since
  Kraken's `subscribe` payload carries the WS auth token in-body (not a
  header, see `exchanges/kraken.md`) — journaling it verbatim would put a
  live credential in a file that gets archived to the NAS. If outbound
  traffic ever needs auditing, log it separately with the token field
  redacted, not folded into the wire-format journal.
- **File format has a header and per-record integrity check** — a magic
  number + format version in the file header (so the format can evolve
  without guessing), and a length trailer or checksum per record. Without
  this, a process crash mid-write leaves a truncated tail indistinguishable
  from corruption, and a reader has no way to detect it and stop cleanly
  short of the bad record. A reader treats the first invalid record as
  end-of-valid-data, not an error to propagate.
- **Capture timestamp is both monotonic and wall-clock**: a wall-clock
  anchor (`CLOCK_REALTIME`) recorded once per file/incarnation, plus a
  monotonic (`CLOCK_MONOTONIC`) reading per record for ordering/latency
  deltas within that incarnation. Monotonic-only can't be correlated across
  a restart or against exchange-side timestamps, so it can't stand alone.
  Known limitation for v1: with IXWebSocket owning the socket, the
  timestamp is taken when our callback receives the frame, not at the
  kernel `recv()` — it silently includes whatever queuing the library does
  internally. Tightening this (e.g. `SO_TIMESTAMPING`) is future work, more
  realistically reachable once the hand-rolled WS client
  (`decisions/0002`) replaces IXWebSocket.
- A **reconnect is an explicit record**, not something inferred later from
  message content — it marks "new connection incarnation, fresh snapshot
  follows," so anything reading the journal never has to guess.
- The capture sequence number is the seam Replay-mode determinism will need
  once multiple exchange threads feed one journal — today it is trivially
  "write order" per connection, but adding it now avoids retrofitting a
  global ordering scheme onto an already-running journal format later.
- **Backpressure policy**: the journal write happens synchronously on the
  connection's own thread (see threading model above), so a slow disk stalls
  that connection's reads. v1 accepts this rather than dropping messages —
  a buffered (1 MiB userspace buffer), non-per-record-fsync'd `ofstream`
  should keep up at Kraken single-symbol message rates as implemented. File
  preallocation (`fallocate`) is **not** implemented in v1, despite earlier
  drafts of this ADR describing it as part of the policy — deferred
  deliberately until there are real throughput measurements to justify it,
  same as everything else in this bullet. This is a real risk if message
  volume grows (a stalled read can make Kraken treat the client as a slow
  consumer and drop it) and should be revisited with actual measurements
  once there's traffic to measure, not assumed away.

### Per-exchange snapshot/recovery/gap handling

- **Kraken**: `subscribe` with `snapshot: true` returns a full L3 snapshot
  then incremental `add`/`modify`/`delete` on the same TCP connection — no
  sequence numbers, so no CME-style gap detection problem (confirmed in
  `decisions/0001`). On disconnect, reconnect and resubscribe with
  `snapshot: true` again; the fresh snapshot is the recovery mechanism, not
  a replay-from-sequence-number request (Kraken doesn't offer one). The
  per-message `checksum` field is **not validated at this stage** — it's
  only computable over a reconstructed book, which doesn't exist until the
  order book lands. This is a deliberate deferral, not an oversight. Token
  refresh, nonce, and reconnect-rate-limit specifics for Kraken are tracked
  in `exchanges/kraken.md`, not here.
- **Staleness watchdog, independent of transport defaults**: a half-open TCP
  connection produces silence, not an error — IXWebSocket's own heartbeat
  is off by default, so relying on it would be relying on an off-by-default
  setting nobody deliberately turned on. v1 needs an application-level
  timer: no message (including heartbeats) within N seconds forces a
  reconnect (new connection incarnation), regardless of what the transport
  library does or doesn't do on its own.
- **Deribit (FIX)**: two independent sequencing layers — FIX session-level
  `MsgSeqNum` (transport reliability) and MD-level
  `MDUpdateAction`/full-refresh-vs-incremental (business-level). Standard FIX
  repairs a session-level gap with `ResendRequest`/`SequenceReset`; **v1
  deliberately does not implement that**, and an earlier draft of this bullet
  describing it as the recovery mechanism here was wrong. A detected gap means
  the session is no longer trustworthy, so the response is the same as
  Kraken's: drop the connection, re-logon, take a fresh snapshot. That keeps
  one recovery story across both exchanges instead of two, and gap-fill is
  only worth building if a real session turns out to gap often enough that
  reconnecting is too expensive. Neither exchange hands this feed handler
  multiple incremental streams to merge, so there's no fan-in-gap-merging
  problem to design for at this stage.
- Instrument reference data (Kraken `GET /0/public/AssetPairs`) is fetched
  once at startup via REST and cached in memory — not part of the streaming
  journal.

### Journal format v1: as implemented (2026-09-16)

The byte layout was left to the implementation; what landed is documented in
full in `src/feed_handler/journal_format.h` (the single definition of every
offset, shared by writer and reader). The choices worth recording here:

- 64-byte file header: magic `CTJOURNL`, `uint16` format version, header
  size, the `CLOCK_REALTIME`/`CLOCK_MONOTONIC` anchor *pair* (sampled
  together, so per-record monotonic readings convert to wall clock), a
  16-byte exchange tag, the incarnation ordinal, and a CRC-32 over the
  header itself. All integers little-endian, written shift-by-shift rather
  than by struct punning.
- 24-byte record header (type, payload length, capture sequence, monotonic
  timestamp) + payload + 4-byte CRC-32 **trailer covering header and payload
  together**. Chose a checksum over a bare length trailer: it catches a
  corrupted body, not just a torn length field, and zlib's `crc32()` was
  already a transitive dependency via IXWebSocket's `USE_ZLIB` so it cost no
  new dependency.
- The reconnect marker is a distinct `record_type`
  (`connection_incarnation`), payload = a free-form reason string. It is
  written through the same stamped-frame path as wire data, so it takes its
  place in the same capture sequence rather than sitting outside the
  ordering.
- The writer **refuses** a frame whose capture sequence is not greater than
  the last one written, and latches an error rather than appending it.
  Silently writing an out-of-order record would break exactly the ordering
  guarantee the sequence number exists to provide, and it would be
  undetectable at replay time.
- Keeping outbound traffic out of the journal is enforced structurally
  rather than by a check: `journal_writer` is a pure sink with no outbound
  path at all, so there is nothing for the WS client to accidentally call.

### WebSocket client v1: as implemented (2026-09-16)

`src/feed_handler/kraken/kraken_ws_client.*` plus the `kraken_feed_handler`
binary. The choices worth recording:

- **IXWebSocket's own automatic reconnection is used as-is** rather than a
  hand-rolled retry loop: it already implements exponential backoff with
  jitter, which is what `exchanges/kraken.md` asks for. Its *defaults* are
  not used, though — the default minimum wait between retries is 1ms, which
  would retry the signed `GetWebSocketsToken` REST call far faster than
  Kraken's rate limits allow. Bounds are set explicitly (1s/30s).
- **Every connection setup is rate-limited by this client, not by the
  library.** Confirmed live: IXWebSocket's reconnect bounds only apply
  between *failed* connection attempts, so a connection that succeeds and is
  then torn down (by the staleness watchdog, or by the exchange) is
  re-established instantly, with no backoff at all — and each one costs a
  fresh signed `GetWebSocketsToken` call. A watchdog flapping against a
  half-broken connection would therefore hammer REST through a socket that
  keeps connecting fine. The client keeps its own floor (the same minimum
  wait) on how often it will set a connection up, and a failure *after* the
  socket connects (token fetch or subscribe send) additionally backs off
  exponentially.
- **Forcing a reconnect is `close()`, never `stop()`.** `stop()` joins the
  library's thread and ends automatic reconnection permanently — it is
  correct exactly once, at process shutdown. The staleness watchdog uses
  `close()` so the library's own reconnect path resumes.
- **The watchdog disarms itself the moment it fires**, and rearms on the
  first inbound frame of the next connection. Otherwise it stays stale for
  the whole reconnect and tears down each new connection before it has had
  a chance to deliver anything.
- Both liveness mechanisms are on: `setPingInterval` (off by default in the
  library, hence set deliberately) *and* the independent application-level
  timeout this ADR requires. Transport-level ping/pong frames count as
  proof of life for the watchdog but are **not journaled** — they are
  IXWebSocket protocol frames, not Kraken wire messages, and the journal
  holds the exchange's own encoding verbatim.
- **Inbound messages are journaled before they are classified.** Classification
  is the barest minimum needed to log a rejected subscribe (a real failure
  mode: `exchanges/kraken.md` confirms the connection stays open and just
  never delivers data), and nothing about a message this build fails to
  understand may cost it a journal record.
- **Failing to open a journal file is fatal to the process**, unlike a
  failed connection. Staying connected while unable to capture would
  silently discard the data the process exists to collect.
- Incarnation bookkeeping lives in `capture_session` rather than in the
  WebSocket callback, so the rotation logic (new file, incremented
  incarnation, marker record, reset sequence numbering) is testable without a
  socket. Files are named `<exchange>-<incarnation>-<UTC timestamp>.journal`:
  the incarnation is what the format cares about, the timestamp keeps
  separate process runs (which all start counting at 1) from colliding.

### Kraken nonce persistence: as implemented (2026-09-16)

`persistent_nonce_source` in `src/feed_handler/kraken/kraken_signing.*`, wired
into `kraken_feed_handler` as `state/kraken-nonce.state` (gitignored, same
reasoning as `journal/`). Kraken's strictly-increasing-nonce rule is
per-API-key and *not* self-healing — one nonce below a value already used
rejects every later call with that key until the clock catches back up — so a
restart landing on a backwards clock step is worth covering even though the
microsecond wall clock handles it in every ordinary case. The choices worth
recording:

- **Persistence wraps `nonce_generator`, it is not inside it.** The generator
  stays a pure `max(now, last + 1)` rule with an injectable "now" and no I/O,
  which is what makes the interesting cases (same-microsecond bursts,
  backwards NTP steps) directly unit-testable; the wrapper adds a seed on
  construction and a write per issuance and nothing else. Persistence is
  opt-in by path — default-constructed, the wrapper is byte-for-byte the old
  behavior, so nothing that does not ask for a file pays for one.
- **Neither the file nor the clock is trusted alone.** Startup seeds the
  high-water mark, not the next value, so the existing rule decides progress
  and yields `max(persisted + 1, now_micros)`.
- **Written on every issuance, not batched.** A signed call happens at most
  once per (re)connect, so batching buys nothing measurable and a batched mark
  is precisely the mark that is stale after a crash.
- **Temp file + rename rather than fsync.** The failure that matters is a
  *smaller* mark surviving, which a torn truncating write can produce and a
  rename cannot. Full durability is deliberately not bought: the wall clock
  still covers the case where the file is lost entirely.
- **Every file error is a warning, never a startup failure** — missing (the
  ordinary first run), corrupt, or unwritable all fall back to today's
  clock-only path. The opposite of the journal rule, and for the opposite
  reason: failing to journal loses the data the process exists to collect,
  whereas failing to persist a nonce only removes a backstop for a case the
  clock almost always already handles.
- Still unsolved on purpose: **two processes sharing one API key.** That needs
  a shared mark (or one key each), and a local file does not pretend to give
  it.

### Deribit FIX session layer: as implemented (2026-09-16)

`src/feed_handler/fix/fix_message.*` (generic FIX.4.4 tag=value mechanics) and
`src/feed_handler/deribit/deribit_fix_session.*` (Deribit's Logon /
MarketDataRequest / Heartbeat construction and sequence tracking). Pure logic;
the raw socket and the `capture_session` wiring are a separate slice. The
choices worth recording:

- **Split generic from exchange-specific.** The envelope arithmetic, the
  framer and the field parser know nothing about Deribit, and the Deribit half
  knows nothing about sockets. That split is what makes the first live Logon
  debuggable: if the session is rejected, the envelope was already verified
  offline against a reference implementation, so the fault is in the
  credentials or the field set, not the framing.
- **The framer finds a message's end from `BodyLength`, never by searching for
  `10=`.** Those three bytes occur naturally inside prices and ids, so
  scanning for them splits messages in the wrong place on real data.
- **Framing and validation are separate steps.** The framer delimits bytes;
  `parse_message` validates `BodyLength` and `CheckSum`. A structurally
  delimited message with a bad checksum is therefore handed out and then
  rejected, rather than silently swallowed by the framer — the client gets to
  decide what a bad checksum means for the session. Framer errors are sticky
  and there is deliberately **no resynchronisation** by hunting for the next
  `8=FIX.4.4`: once framing is lost, every byte after it is unaligned, and the
  answer is the same reconnect the gap policy above prescribes.
- **Repeating groups are not parsed into a structure in v1.** `parse_message`
  produces a flat, ordered field list; `get()` returns the first occurrence of
  a tag, which is correct for top-level session fields and wrong for a group
  member. Building groups outbound is unaffected (FIX groups are positional,
  so an ordered field list is exactly right). This is a real corner cut, and
  it has to be closed before `35=W`/`35=X` book content can be read — it is
  flagged in `fix_message.h`, and the ordered list is kept precisely so a group
  parser layers on without re-parsing.
- **A detected sequence gap is reported, not repaired**, per the corrected
  bullet above. The expectation advances past the gap rather than staying put,
  so one lost message produces one gap report instead of an identical report
  on every subsequent message while the reconnect is still in flight.
  `PossDupFlag(43)=Y` is exempt from the check: an administrative resend
  legitimately repeats a sequence number and must not tear down a healthy
  session.
- **The Logon password hash is pinned against two independent
  implementations**, not against itself: a Python `hashlib`/`base64` known
  answer for a fixed fake timestamp/nonce/secret, and a whole-message byte
  comparison against `simplefix`'s own `encode()` — the library the probe
  Deribit's testnet actually accepted was written with. Same discipline that
  caught the Kraken HMAC bugs.
- **base64/SHA-256 are re-implemented in the Deribit TU rather than shared
  with `kraken_signing`.** A Deribit translation unit that has to include a
  Kraken header to log on is exactly the coupling this second backend exists
  to catch. If a third user appears, promote them to a shared helper — do not
  let one exchange depend on another.

### Deribit FIX client v1: as implemented (2026-09-16)

`src/feed_handler/deribit/deribit_fix_client.*` plus the `deribit_feed_handler`
binary: the raw socket under the session layer above, wired to the same
`capture_session` Kraken uses. The choices worth recording:

- **One dedicated thread doing a blocking `recv()`, not epoll.** The
  epoll-per-thread-group model above is the end-goal for when there is more
  than one connection to *group*; with exactly one Deribit socket a
  multiplexer would be machinery with nothing to multiplex. This is the same
  scope as Kraken's v1 (one thread per connection), just hand-rolled instead
  of library-provided — and because this client owns its fd outright, it can
  join an epoll group the day one exists, with no library in the way.
- **`SO_RCVTIMEO` is what makes one thread enough.** The session needs two
  timers (the outbound Heartbeat every `HeartBtInt`, and the staleness
  watchdog) and a shutdown check, and a receive timeout gives all three a tick
  without a second thread or a readiness API. A timed-out `recv` is not an
  event, it is the loop's clock. Kraken needed a separate watchdog thread only
  because IXWebSocket owns its own loop and offers no such hook.
- **`connect()` is non-blocking + `poll()`, then back to blocking.** Not for
  concurrency — purely so an unreachable host cannot park the thread for the
  kernel's own multi-minute SYN timeout and make shutdown look hung.
- **Staleness timeout is 90s against a 30s `HeartBtInt`**: three missed
  heartbeats. Deliberately loose, because regular heartbeats *are* the
  steady-state traffic on an idle book and the watchdog's job is to catch a
  half-open socket, not to police jitter.
- **Backoff advances on "this connection never delivered a message", not on
  "connect() failed".** A TCP connect that succeeds and is then dropped
  seconds later (a rejected Logon, say) is still a failure, and resetting the
  counter on connect alone would spin on it — the same trap Kraken's
  `throttle_connection_setup` exists for, arrived at from the other direction.
  This client has no library reconnect layer underneath, so its backoff is the
  only one, which makes it simpler than Kraken's two-layer arrangement, not
  more complex.
- **A rejected `MarketDataRequest` (`35=Y`) is loud but is not a reconnect.**
  Unlike a gap, the session is healthy; reconnecting would only replay the
  same rejected request. Confirmed in `exchanges/deribit.md`: there is no
  positive ack for the request either, so the snapshot's *absence* is the only
  other symptom.
- **A peer-initiated `Logout` (`35=5`), a framing loss and a failed
  `BodyLength`/`CheckSum` validation all take the gap path** — one recovery
  story, as the gap-handling bullet above requires.
- **The whole "what happens next" decision is a pure function**
  (`classify_inbound`), taking the parsed message and the session's sequence
  verdict. That is what keeps the branches a live socket makes hardest to
  reach — a gap, a peer Logout, a rejected subscribe on an otherwise healthy
  session — unit-testable, exactly as `capture_session` and
  `staleness_watchdog` were on the Kraken side. Socket I/O itself is proven by
  the live testnet run, not by a mocked fd.
- **Keeping outbound bytes out of the journal stayed structural.**
  `on_wire_message` is only ever called with a `framer::next_message()` view,
  and the Logon/Heartbeat/MarketDataRequest builders return strings that go
  straight to `send()`. Nothing had to be remembered.
- **`capture_session` and `staleness_watchdog` were reused unchanged.** The
  only exchange-specific thing either needed was the string `"deribit"`. That
  is the result the Kraken-first/Deribit-second ordering in Consequences was
  designed to test, and the seam held: the two exchanges differ in transport
  (library-owned WSS vs. hand-rolled TCP), encoding (JSON vs. tag=value),
  recovery trigger (silence vs. `MsgSeqNum`) and liveness mechanism, and share
  the journal layer verbatim.

## Consequences

- Kraken-first, Deribit-second implementation order is intentional: it
  forces the `MessageSink`/per-connection-thread abstractions to prove
  they're not secretly Kraken-shaped before more code is built on top.
- Checksum-based desync detection and any unified/lightweight internal
  message schema both remain explicitly deferred (the latter per
  `decisions/0002`'s serialization deferral) — revisit both once an order
  book exists to validate against.
- Matching and self-match prevention are out of scope for LiveTrading/Replay
  order books entirely; only the Simulation-mode fake exchange matches.
- This ADR does not yet cover: the order book itself, strategy/order-entry
  hookup, or the concrete SPSC queue placement for multithreading — each is
  a future ADR once there's a concrete design to lock in rather than
  speculate about.
- **Replay resync is intentionally underspecified here.** Reconstructing
  "the same order the strategies originally saw" needs a manifest ordering
  incarnation files (directory listing order isn't a safe substitute) and an
  explicit pacing contract (as-fast-as-possible vs. reproducing recorded
  inter-arrival gaps) — both are real design questions for the Replay-mode
  ADR when that mode is actually built, not resolved by this one.
- Sink dispatch mechanism (virtual vs. compile-time policy) is likewise left
  open per the Decision section above — flagged here so it isn't mistaken
  for a settled choice.

## Review history

An independent design review (2026-09-16, Opus-model advisor agent, no
access to this conversation's context — read only the checked-in
ADRs/exchange docs/probes) caught the IXWebSocket-vs-shared-reactor
contradiction, the per-symbol journal file layout breaking arrival order,
the missing frame-ownership contract, and several of the other gaps folded
into the sections above. Recorded here so the reasoning behind the
threading-model rewrite in particular doesn't look unmotivated later.
