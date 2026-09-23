---
aliases: [fix, deribit client, deribit fix, framer, repeating groups, fix session]
sources: [src/feed_handler/fix/**, src/feed_handler/deribit/**]
decisions: [decisions/0001-feed-source-selection.md, decisions/0010-connection-thread-handoff.md, decisions/0011-desync-recovery.md]
---

# Deribit FIX client

Three layers, each knowing nothing of the one above it:

- `fix/fix_message.*`: generic FIX.4.4 tag=value building, framing, parsing and
  repeating groups. Knows nothing of Deribit.
- `deribit/deribit_fix_session.*`: Deribit's Logon, MarketDataRequest and
  Heartbeat, and sequence tracking. Pure logic, no sockets.
- `deribit/deribit_fix_client.*`: the raw socket and its thread, wired to the
  same `CaptureSession` Kraken uses.

Wire facts are in `exchanges/deribit.md`. The split is what made the first live
Logon debuggable: the envelope was already verified offline against a reference
implementation, so a rejection meant credentials or fields, not framing.

## Framing and parsing

- **The framer finds a message's end from `BodyLength`, never by searching for
  `10=`.** Those bytes occur inside prices and ids.
- **Framing and validation are separate.** The framer delimits; `ParseMessage`
  checks `BodyLength` and `CheckSum`, so a delimited message with a bad checksum
  is handed out and rejected rather than swallowed, and the client decides what
  it means for the session.
- **Framer errors are sticky, with no resynchronisation** by hunting for the
  next `8=FIX.4.4`: once framing is lost every later byte is unaligned, and the
  answer is a reconnect (`decisions/0011`).
- The parser produces one flat, ordered field list; `Get()` returns a tag's
  first occurrence, right for session fields, meaningless for group members.

## Repeating groups

`ReadGroup()` reads a group on top of the flat list, without re-parsing:

- **A repetition ends where the delimiter tag comes round again**, never after a
  fixed number of fields. The delimiter is whatever tag follows `NumInGroup`
  (`279` on a `35=X` entry, `269` on a `35=W` one). A fixed stride would
  desynchronise the whole group the first time an optional member is omitted,
  which is legal; a test omits one `272` mid-group for this.
- **The caller passes the member tags**, so `35=W` and `35=X` are two shapes, and
  the first non-member tag ends the group. Asking an entry for a tag it does not
  carry returns `nullopt`.
- **A `NumInGroup` that lies is salvaged and flagged**: fewer repetitions than
  declared yields those present with `Truncated()` true. Nothing is sized from
  the declared count. Extra repetitions stay outside the group.
- **Nested groups are unsupported**, because knowing which tag opens a sub-group
  needs a data dictionary, and no group either exchange sends nests
  (`fix_message.h` says so).

## Session

- **A detected sequence gap is reported, not repaired** (`decisions/0011`). The
  expected sequence advances past the gap, so one lost message is one report,
  not one per message until the reconnect lands. `PossDupFlag(43)=Y` is exempt:
  an administrative resend legitimately repeats a number.
- **The Logon password hash is pinned against two independent
  implementations**: a Python `hashlib`/`base64` known answer, and a
  whole-message comparison with `simplefix`'s `encode()`, the library of the
  probe Deribit's testnet accepted.
- **base64 and SHA-256 are re-implemented here rather than shared with
  `kraken_signing`**, so a Deribit translation unit never includes a Kraken
  header. If a third user appears, promote them to a shared helper.

## Client

- **One thread doing a blocking `recv()`, not epoll.** With one socket there is
  nothing to multiplex. The client owns its fd, so it can join an epoll group
  when one exists (`decisions/0010`).
- **`SO_RCVTIMEO` is the loop's clock.** A timed-out receive gives the watchdog
  and shutdown checks a tick without a second thread. The outbound Heartbeat
  check runs on every iteration regardless (`SendHeartbeatIfDue`), because a busy
  connection whose `recv()` always returns promptly would otherwise never send
  one; that was a real bug.
- **`connect()` is non-blocking plus `poll()`, then blocking again**, so an
  unreachable host cannot park the thread for the kernel's multi-minute SYN
  timeout and make shutdown hang.
- **Staleness timeout is 90 s against a 30 s `HeartBtInt`**: three missed
  heartbeats. Loose on purpose; heartbeats are the steady-state traffic of an
  idle book, and the watchdog is for half-open sockets, not jitter.
- **Backoff advances on "this connection never delivered a message"**, not on
  "`connect()` failed": a TCP connect followed by a rejected Logon is still a
  failure, and resetting on connect would spin. There is no library reconnect
  layer underneath, so this backoff is the only one.
- **A rejected `MarketDataRequest` (`35=Y`) is logged loudly but is not a
  reconnect.** The session is healthy, and reconnecting would replay the same
  request. There is no positive ack either, so a missing snapshot is the other
  symptom.
- **A peer `Logout` (`35=5`), framing loss and failed validation all take the gap
  path**: one recovery story.
- **What happens next is a pure function**, `ClassifyInbound`, of the parsed
  message and the session's sequence verdict, so the branches a live socket
  makes hardest to reach (gap, peer Logout, rejected subscribe) are unit-tested.
  Socket I/O is proven by the live testnet run and a loopback harness.
- **The capture session is closed on every exit from the read loop**, through an
  RAII guard rather than a `close()` before each of a dozen returns, so a gap or
  a lost socket leaves a complete, readable file.
- **Outbound bytes stay out of the journal structurally**: only views from the
  framer reach `OnWireMessage`, and the message builders return strings that go
  straight to `send()`.

## Testing

The loopback harness can stop listening mid-test. That is what makes "the
journal was closed" observable: with the listener up the client reconnects at
once, and the next connect would close the file regardless. "Closed" is asserted
as "reads back in full, at clean EOF", since records sit in the userspace buffer
until something flushes it.
