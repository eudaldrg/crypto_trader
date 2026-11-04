# 0010. Connection threads hand frames off through one ring per consumer

- Status: proposed
- Date: 2026-09-23

## Context

A connection thread that stops reading loses data: Kraken drops slow consumers,
and a stalled FIX session misses its heartbeats. Writing a journal and building
books both take variable time (a buffer flush is a `write()`; a book update can
hit a slow path). Several consumers want every frame of a connection: the
journal, the books, later strategies. The long-term goal is to give dedicated
cores to parts of the pipeline.

## Options

### A. Everything inline on the connection thread

What v1 did. Simple and deterministic. Hard: a disk flush stalls the socket, and
the journal write sits in front of every book update.

### B. One bounded SPSC ring per (connection, consumer)

The connection thread stamps a frame once, copies it into each consumer's ring
and returns; each consumer thread drains its rings. Easy: no locks, no
contention, per-connection FIFO by construction, each consumer chooses its own
overflow behavior. Hard: one copy (and today one allocation) per frame per ring.

### C. One multi-producer queue per consumer

Fewer queues. Hard: producers contend on one structure, and per-connection order
depends on the queue rather than on construction.

### D. One ring per connection with a read cursor per consumer

One copy instead of one per consumer. Not offered by the ring library in use;
left to profiling.

## Decision

B. A connection thread never blocks on a consumer and never does disk I/O.
Until the target stage below, it does no parsing either. Each consumer owns
what a full ring means for it, and none of those policies may block the
connection thread: for the journal see
`decisions/0012`, for books `decisions/0011`.

Connect and disconnect events go through an unbounded path on every ring and are
never dropped, so a consumer never misses where a connection begins or ends.

Staged, so reaching the target is not a reversal:

- **Now:** one thread per connection (IXWebSocket's own for Kraken, the client's
  for Deribit), one journal thread per connection, one book thread for the
  process.
- **Target:** connection sockets grouped onto epoll threads pinned to cores,
  sharded by core when one is full. Each epoll thread also runs its
  connections' feed books (`decisions/0014`) inline: it parses, validates and
  publishes normalized events, requests its own reconnects, and can serve a
  current book as a snapshot to a restarted consumer (`decisions/0011`). Raw
  frames still reach the journal ring before parsing. Trigger: fast feed books
  exist, since a feed book on the read path must never be slow enough to stall
  the socket, and IXWebSocket is replaced by a hand-rolled WebSocket client,
  since Kraken cannot join an epoll group while IXWebSocket owns its socket and
  thread.

The book adapter has no threads (`decisions/0008`), so moving it from the book
thread onto the connection thread is a wiring change, not a rewrite. The
journal rings stay the same across both stages.

## Consequences

- A consumer's failure is asynchronous: clients register a handler rather than
  reading it from a return value. Today that handler stops every connection,
  which `decisions/0012` changes.
- On one book thread, a slow connection's book work delays the other
  connections' books, never their journaling.
- The order in which the book thread interleaves connections depends on timing
  and is not recorded (`decisions/0009`).
- The copy per frame per ring is the cost of B; whether to cut it is
  `docs/questions/2026-09-23-ring-copies-and-slots.md`.

## Changes

- 2026-10-04: target moved the feed books from sharded book threads onto the
  epoll threads, because ADR 0014 makes the feed book the normalization
  boundary and the thread that validates a feed should own its reconnect.
