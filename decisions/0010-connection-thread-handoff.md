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

B. A connection thread never blocks on a consumer and never does disk I/O or
parsing. Each consumer states what a full ring means for it:

- **Journal: stop journaling that connection, loudly.** It never blocks the
  socket and never silently drops a frame from a file it keeps writing: a full
  ring or a failed write ends that connection's journal and raises an alert,
  and the connection and its books carry on (`decisions/0012`).
- **Books: drop and desync.** Books never fail a capture: a full ring drops the
  frame, and that connection's books stay desynced until they recover
  (`decisions/0011`).

Connect and disconnect events go through an unbounded path on every ring and are
never dropped, so a consumer never misses where a connection begins or ends.

Staged, so reaching the target is not a reversal:

- **Now:** one thread per connection (IXWebSocket's own for Kraken, the client's
  for Deribit), one journal thread per connection, one book thread for the
  process.
- **Target:** connection sockets grouped onto epoll threads pinned to cores, and
  book work sharded by symbol or connection group. Trigger: one book thread
  cannot keep up, or pinning is being measured. Kraken cannot join an epoll group
  while IXWebSocket owns its socket and thread; replacing it with a hand-rolled
  WebSocket client removes that.

The rings are the seam that stays the same across both stages.

## Consequences

- A consumer's failure is asynchronous: clients register a handler rather than
  reading it from a return value. Today that handler stops every connection,
  which `decisions/0012` changes.
- Closing a connection's journal is a barrier: it returns once the journal
  thread has written and closed the file.
- On one book thread, a slow connection's book work delays the other
  connections' books, never their journaling.
- The order in which the book thread interleaves connections depends on timing
  and is not recorded (`decisions/0009`).
- The per-frame allocation is accepted until `journal_replay` shows it matters;
  the fix then is preallocated slots.
