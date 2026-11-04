# 0012. A failure stays inside the smallest unit that owns the broken state

- Status: proposed
- Date: 2026-09-23

## Context

The system is meant to serve several exchanges and several strategies at once,
including strategies that act across exchanges. Components fail at different
scopes: a book desyncs, a connection drops or its journal falls behind, the disk
fills. A strategy can act sensibly on partial data (its derivatives books are
stale, its Kraken books are not) only if a failure is announced at its own scope
and does not take down everything else.

Some failures do threaten the whole process. Running out of disk or memory can
cost the state that matters most once trading exists: logs of orders sent and
confirmations received, and the portfolio state saved for the next run. Market
data journals are worth much less than that.

## Options

### A. Any fatal stops the process

What the code does today (`runner.cpp`: "capture failed, stopping every
connection"). Simple and loud. Hard: one connection's journal falling behind
kills every other capture and every book, and restarting is manual either way.
It also defeats the separate per-consumer rings of `decisions/0010`.

### B. Contain by level, escalate only shared-resource exhaustion

Book < connection < process. A failure is handled and announced at its own
level. Only exhaustion of a shared resource escalates to the process, and then
into a degraded mode rather than a stop.

### C. Never stop

Everything logs and carries on. Hard: a full disk quietly corrupts whatever is
written next, including state needed to restart safely.

## Decision

B.

- **Book:** desyncs on its own (`decisions/0011`); other books are unaffected.
- **Connection:** a protocol loss reconnects that connection only
  (`decisions/0011`). A journal failure on one connection (ring overflow, failed
  write, file that cannot be opened) stops journaling for that connection and
  raises an alert. It never blocks the socket (`decisions/0010`) and never
  silently drops a frame from a file it keeps writing. The socket and its books
  keep running.
- **Staleness is always announced** at the level that went stale
  (`OnDisconnect` per connection, `Desynced` per book), so consumers decide what
  partial data means for them.
- **Process:** only shared-resource exhaustion (disk, memory) escalates, into a
  degraded maintenance mode rather than a stop, which protects what matters more
  than market data. What it sheds, protects and alerts is not decided
  (`docs/questions/2026-09-23-maintenance-mode.md`).

## Consequences

- Today's code contradicts this: any connection's fatal stops every connection.
  Changing it is follow-up work.
- An alert channel is needed, since "keep running and say so" is only as good
  as someone hearing it.
- A connection whose journal stopped keeps capturing nothing to disk until it is
  fixed. Restarting its journal automatically is not decided; it waits until a
  real failure shows what is recoverable
  (`docs/questions/2026-09-23-journal-failure-recovery.md`).
- The maintenance mode needs order entry (#11) and strategies (#10) before it
  can be designed.
