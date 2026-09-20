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
deliberately the *only* seam that changes across modes and threading models.
(The interface as built is not quite the one described here — it also carries a
reconnect notification and a per-frame source identity, and the journal writer
is a special always-present sink rather than one of many. See "MessageSink and
capture_session: as implemented" below, which supersedes this paragraph and the
`on_frame` signature above.)

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
- **Repeating groups are read on top of the flat field list, not baked into
  the parser.** `parse_message` still produces one flat, ordered field list and
  `get()` still returns a tag's first occurrence (right for top-level session
  fields, meaningless for a group member); `read_group()` layers on top of that
  ordering without re-parsing, which is what preserving wire order was for.
  Building groups outbound is unaffected (FIX groups are positional, so an
  ordered field list is exactly right). See the group-reader subsection below
  for the semantics; the only thing still unsupported is *nested* groups, which
  no group on either exchange's wire uses.
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
- **`SO_RCVTIMEO` is what makes one thread enough.** The session needs a
  staleness-watchdog check and a shutdown check on top of whatever `recv()`
  itself is doing, and a receive timeout gives both a tick without a second
  thread or a readiness API. A timed-out `recv` is not an event, it is the
  loop's clock. Kraken needed a separate watchdog thread only because
  IXWebSocket owns its own loop and offers no such hook. The outbound
  Heartbeat timer is checked on every loop iteration regardless — it does not
  wait for a receive timeout, since a busy connection where `recv()` always
  returns promptly would otherwise never reach it (a real bug this project
  had and fixed: see `src/feed_handler/deribit/deribit_fix_client.cpp`'s
  `send_heartbeat_if_due`).
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

### FIX repeating groups: as implemented (2026-09-16)

`fix::read_group()` in `src/feed_handler/fix/fix_message.*` closes the corner
cut recorded in the session-layer section above. It is parsing capability only:
nothing consumes book content yet, and wiring it into the client's
classification path waits for the order book. The choices worth recording:

- **A repetition ends where the delimiter tag comes round again, never after a
  fixed number of fields.** The delimiter is whatever tag follows NumInGroup,
  so it is discovered rather than declared — `279` on a `35=X` entry, `269` on a
  `35=W` one. A fixed stride would look right against the captured messages and
  desynchronise the entire group the first time an exchange omits an optional
  member, which is legal and needs no announcement. There is a test that omits
  one `272` mid-group for exactly this reason.
- **The entry shape is a parameter, not a guess.** The caller passes the member
  tags, so `35=W` and `35=X` are two calls with two shapes rather than one
  union that quietly tolerates both; the member set is what says where the
  group *ends* (the first non-member tag). Per-entry lookup still returns
  `nullopt` for a tag this entry does not carry, matching `parsed_message::get`,
  because asking a snapshot entry for its `MDUpdateAction` is a fair question
  with the answer "none".
- **A NumInGroup that lies is salvaged and flagged, not rejected.** Fewer
  repetitions present than declared yields the ones that are there with
  `truncated()` true — the same split as framer-vs-parser: hand out what was
  structurally recoverable, make the defect unmissable, let the client decide
  what it means for the session. Nothing is sized or reserved from the declared
  count, so a nine-digit NumInGroup costs a bounded scan rather than an
  allocation. In the other direction NumInGroup is authoritative: extra
  repetitions on the wire stay outside the group, so a stray member-tagged
  field cannot be absorbed into it.
- **Nested groups remain unsupported, deliberately.** Knowing which member tag
  opens a sub-group needs a data dictionary, and no group either exchange sends
  (`NoMDEntries`, `NoMDEntryTypes`, `NoRelatedSym`) nests. That is the whole of
  the remaining limitation, and it is stated as such in `fix_message.h` rather
  than left as a general "groups are not supported" warning.

### MessageSink and capture_session: as implemented (2026-09-16, revised)

An independent code review found that the seam this ADR describes was not
actually the seam that got built, and that both clients had a capture bug on a
failure path. This subsection is the corrected shape; where it contradicts the
Decision section above, **this is what the code does**.

What the earlier text claimed and the code did not do:

- "Every exchange connection emits capture frames into a `MessageSink`" was not
  true of the implementation. `capture_session` held a concrete
  `journal_writer` and both clients called `capture_session::on_wire_message`
  directly; nothing ever went through the interface, and there was no way to
  register a second sink at all. A second sink was not "a later addition", it
  was impossible.
- The one event a second sink cannot be correct without — a reconnect — was not
  on the interface either. `write_incarnation_marker` was a `journal_writer`
  method, so an order book had no way to learn that it must reset.
- A frame carried no identity: nothing on a `capture_frame` said which exchange
  or wire encoding produced it, so a sink fed by both clients would have had to
  re-derive that from the raw bytes.

What now exists:

- **`message_sink` gains `on_incarnation(incarnation, reason)`** alongside the
  pure-virtual `on_frame`, with a no-op default body. Most sinks have no state
  to reset; an order book has nothing but.
- **The incarnation marker record and the incarnation notification stay two
  different things.** `journal_writer` keeps `write_incarnation_marker(frame)`
  as its own concrete method, called directly by `capture_session`, and does
  *not* override `on_incarnation`. The marker is a *record*: it needs a stamped
  frame so it takes its place in this incarnation's capture sequence, and the
  session's single `capture_stamper` is the only thing entitled to hand out a
  sequence number. Routing it through `on_incarnation` instead would mean the
  writer stamping its own frames from a second sequence source, which is
  exactly what a single stamper exists to prevent. The notification form needs
  no sequence number at all, so the two do not collapse into one call.
- **`capture_session::add_sink(message_sink&)` registers additional non-owning
  sinks** (a small `std::vector<message_sink*>`), which receive both `on_frame`
  and `on_incarnation`. The journal writer is deliberately not one of them: it
  is the always-present sink that makes capture durable, it is the only one
  whose failure `on_wire_message` reports, and it always goes first — the same
  "journal first, classify second" discipline both clients already follow, one
  level down, so nothing a downstream sink does can decide whether a record is
  written. Extra sinks *do* still see a frame whose journal write failed: what
  failed is the disk, not the data. Sinks are told about an incarnation only
  once it is actually usable, since every caller treats a failed
  `begin_incarnation` as fatal to capture.
- This is all the fan-out there is, on purpose: same thread, same call, no
  queue. The cross-thread fan-in seam above is unchanged and still future work.
  It will change what a sink does inside `on_frame`, not this call — which is
  the whole point of the frame-ownership contract.
- **`capture_frame` gains `frame_source source`** — an enum naming the wire
  shape (`kraken_json`, `deribit_fix`, `unknown`), not just the exchange,
  because what a sink has to decide is which parser the payload goes to. It is
  supplied by the *client*, at the `on_wire_message`/`begin_incarnation` call
  site, rather than configured on `capture_session`: the client is the only
  thing that knows first-hand what it just received, whereas a session
  configured by the binary that owns it could be handed the wrong answer and
  nothing would notice until an order book parsed JSON as tag=value.
- **`frame_source` is not in the journal format and needs no version bump.** A
  journal file is one per (exchange, connection-incarnation) and its header
  already carries the exchange tag, so a replay source recovers this once per
  file rather than once per record.

Two capture bugs fixed with it, both on the path an order book would sit on:

- **Kraken now treats a failed journal write as fatal**, as Deribit already
  did. It previously only logged, so after (say) a full disk the
  `while (... && !client.fatal())` loop in `kraken_feed_handler` never noticed
  and the process ran "healthy" while capturing nothing — the exact outcome the
  "failing to open a journal file is fatal" rule exists to prevent, arrived at
  from the other direction. A message arriving *before* the first incarnation
  is deliberately still not fatal: that one is recoverable on the next connect.
  Kraken's `wait_for_stop` predicate now includes `fatal()` too; the fatal
  paths were already calling `notify_all()` on that condition variable, but a
  predicate that only looked at `stopping_` sent the woken waiter straight back
  to sleep for the rest of its timeout.
- **Deribit now closes the capture session on every exit from its read loop**,
  via an RAII guard rather than a `close()` before each `return` (there are a
  dozen, and the next one added would have been missed). Previously *no* path
  closed it: a gap, a peer Logout, framing loss, a `recv()` error and a
  staleness reconnect all left the file open with up to a full 1 MiB write
  buffer unflushed until the next successful connection's `begin_incarnation`
  closed it — up to `max_reconnect_wait_ms` (30s) later, or never if the
  exchange stayed down. Kraken already did the equivalent on its `Close` event,
  for the reason its comment gives: a closed file is a complete, readable one.

Testing notes worth keeping:

- The Deribit loopback harness gained the ability to stop listening mid-test.
  That is what makes "the journal was closed" observable at all: with the
  listener still up, the client reconnects immediately and the *next*
  `begin_incarnation` closes the previous file regardless, so the test would
  pass either way. With nothing to reconnect to, the only thing that can have
  closed the file is the path under test. "Closed" is asserted as "reads back
  from disk in full, at clean EOF" rather than as a counter, since records sit
  in the userspace buffer until something flushes it.
- Kraken has no equivalent live-socket harness (IXWebSocket owns its thread and
  fd, and a real connection would also need a signed REST token call), so
  `ws_client::handle_message` is public and driven directly, in the same spirit
  as `rest_client::parse_asset_pairs`. The journal failure it needs is produced
  by latching the writer's sticky error with an oversized record, which is the
  same sticky state a full disk leaves behind.

### Configuration and capture scope (2026-09-20)

**Configuration is a TOML file, one `[[connections]]` entry per socket.** The
binaries used to hardcode one symbol each; they now take `--config <path>`
(required, not guessed from the working directory) and open every entry for
their own exchange. TOML over YAML/JSON/XML because its array-of-tables is
exactly "a list of connection entries", its scalars are unambiguous, and it
allows comments; `toml++` is header-only and comes in through `FetchContent`
like GoogleTest. One entry is one socket and one journal, which keeps this ADR's
"one file per (exchange, connection-incarnation), not per symbol" rule intact
and makes a second Deribit instrument or a shard past Kraken's 200 symbols a
config edit. Credentials are named by environment variable, never stored (the
schema and its validation are in `docs/modules/feed-handler.md`).

**Multi-symbol is one subscribe per connection.** Kraken's `symbol` param is an
array; Deribit's `NoRelatedSym(146)` is a repeating group. Both verified live
(`exchanges/kraken.md`, `exchanges/deribit.md`). Neither has a wildcard, so
symbols are always enumerated.

**Initial capture scope: 5 to 10 symbols per exchange, one connection each, raw
and uncompressed.** Measured from real captures in this repo:

| | rate | per day |
|---|---|---|
| Kraken `BTC/USD` `level3` alone | ~50 msg/s, ~17.8 KiB/s | ~1.5 GiB |
| Deribit `BTC-PERPETUAL` alone | ~9.6 msg/s, ~2.4 KiB/s | ~0.2 GiB |
| Kraken `BTC/USD` + `ETH/USD` (30 s sample, 2026-09-20) | ~28 KiB/s of journal | ~2.3 GiB |

Scaled against Kraken's 24h trade counts (`BTC/USD` is about 6.2% of all-pair
trades, the top five about 21%) and Deribit's ~5,540 active instruments (~5,100
of them BTC/ETH options), the options are: (a) BTC only on both exchanges,
~2 GiB/day; (b) the top five per exchange, ~6 GiB/day; (c) everything, ~40 to
90 GiB/day (1 to 3 TB a month). (c) buys no extra portfolio story over (b), for
all of the cost, so (b) is the target. The ETH sample ran at several times
`BTC/USD`'s update rate, so per-symbol volume varies widely and (b) is an
estimate to re-measure once it is running, not a budget.

Deliberately not done: the journal stays raw text per this ADR's format, so a
schema or compression pass is later work; this capture exists to collect a couple
of days of real data to design that against. Subscribe pacing (needed past about
40 Kraken symbols at depth 10) and connection sharding (past 200) wait until the
symbol count gets near either.

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
