# 0004. Exchange-facing code emits raw frames through one sink seam

- Status: proposed
- Date: 2026-09-23

## Context

The project needs three execution modes that strategy-facing code cannot tell
apart: **LiveTrading** (real sockets), **Replay** (a captured session, replayed
deterministically for debugging) and **Simulation** (a fake exchange replaying
captures with matching books and an order-entry hook). Only the code nearest the
exchange differs between them: a live socket, a journal reader, a fake exchange.
That only works if what crosses the boundary between "exchange-facing" and
"everything else" is fixed before much is built on either side of it, because
every consumer (the journal, the books, later strategies) is written against it.

## Options

### A. Raw wire frames, non-owning, sinks copy what they keep

Each connection hands every inbound message to its sinks as the exchange's own
bytes plus capture metadata. The bytes are a view valid only for the duration of
the call. Easy: no parsing before capture, so a message this build cannot
understand is still captured; the journal is just another consumer; nothing is
allocated for a sink that does not keep the frame. Hard: every consumer parses
for itself, and one that keeps a frame (a ring to another thread) must copy it.

### B. Parsed, normalized events at the seam

Clients parse and emit a unified event type. Easy for consumers. Hard: the
unified schema has to exist before anyone knows what the consumers need, a
parser bug becomes a capture bug, and anything journaled downstream of the seam
has lost the wire detail.

### C. Owning frames (reference-counted buffers)

Sinks may keep a frame without copying. Hard: an allocation and a refcount on
every frame for every sink, including the ones that never keep it, and buffer
lifetimes silently cross threads.

## Decision

A, because capture must not depend on parsing, and the copy a cross-thread
consumer needs is then made once, at the one place that knows it needs it (its
ring's push, `decisions/0010`).

The seam is `MessageSink` (`src/feed_handler/message_sink.h`): `OnFrame(frame)`,
plus `OnConnect(connect_id, reason)` and `OnDisconnect(connect_id)` so a stateful
sink learns where a connection begins and ends (`decisions/0011`). A frame carries
the payload view, a per-connection capture sequence number, a monotonic
timestamp, and a `FrameSource` naming the wire encoding, set by the client that
received it.

`connect_id` is the boundary sinks reset on: it numbers the established
connections of one configured connection, 1 for the first and plus one per
reconnect, restarting each process run. Every stateful consumer (journal file,
books, later strategies) starts over at a new one.

## Consequences

- Replay is a journal reader driving the same calls in the captured order; the
  consumers do not change.
- Parsing lives downstream of the seam, per consumer.
- That the seam is not Kraken-shaped has been tested by building Deribit on it
  (`docs/modules/feed-handler.md`).
- Not constrained here: whether dispatch is virtual or a compile-time policy
  (virtual today; `docs/questions/2026-09-23-sink-dispatch-mechanism.md`), and
  what the journal stores (`decisions/0009`).

## Changes

- 2026-09-16: `OnConnect` and `FrameSource` added, after a review found the
  built seam was not the one described (nothing went through the interface, and
  a reconnect was invisible to sinks).
- 2026-09-21: `OnDisconnect` added, so a book goes stale when its feed drops.
- 2026-10-04: the `connect_id` definition moved here from `decisions/0011`,
  since the seam is what carries it.
