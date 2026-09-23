# 0011. A connection that lost data recovers by reconnecting for a fresh snapshot

- Status: proposed
- Date: 2026-09-23
- Revisit when: an exchange is added whose snapshot is slow or delayed (a
  CME-style recovery feed), or whose gaps make reconnecting too expensive.

## Context

A feed can lose data in several ways, and each exchange shows it differently:

- a FIX `MsgSeqNum` gap, framing loss, or a failed `BodyLength`/`CheckSum`
  (Deribit);
- a checksum mismatch after applying an update (Kraken);
- silence, because a half-open TCP connection produces no error;
- a frame dropped by a full book ring (`decisions/0010`);
- an integrity issue inside a book: unknown order or level, crossed book.

Kraken offers no replay from a sequence number; its only recovery is a fresh
snapshot, sent on subscribe. Anything downstream of a book has to be able to tell
a book it can trust from one it cannot.

## Options

### A. Repair in place where the protocol allows it

FIX `ResendRequest`/`SequenceReset` for Deribit, resubscribe for Kraken. Keeps a
Deribit session alive through a gap. Hard: two recovery stories, the harder one
for the exchange whose gaps have not been observed yet.

### B. Always reconnect and take a fresh snapshot

Any loss ends *that* connection. Its next connection gets a new `connect_id` and
a fresh snapshot, and everything downstream of it resets at that boundary. One
mechanism for every exchange and every kind of loss; other connections are
untouched. Hard: a reconnect costs a REST token call
(Kraken) and a Logon (Deribit), plus the time to the next snapshot.

### C. Keep applying and flag the book

No reconnect cost. Hard: a book that is wrong looks healthy.

## Decision

B, because both current exchanges deliver a snapshot on (re)subscribe within
seconds, so reconnecting is cheap and a second recovery path buys nothing yet.
Detection and recovery are both scoped to one connection (`decisions/0012`):

- A book that cannot be trusted is `Desynced`, is reported, and stops applying
  updates. It never silently carries on. It becomes `Ready` again only from a
  fresh snapshot, which in practice comes with the next `connect_id`.
- A session-level loss (gap, framing loss, bad checksum, peer Logout) drops the
  connection.
- Liveness is checked by an application-level staleness watchdog on every
  connection, independent of transport heartbeats: no message within the timeout
  forces a reconnect.
- `connect_id` numbers the established connections of one configured
  connection: 1 for the first, plus one per reconnect, restarting each process
  run. It is the boundary sinks reset on (`OnConnect`/`OnDisconnect`,
  `decisions/0004`).

## Consequences

- There is no gap-fill code.
- Target, once the revisit trigger fires: local recovery that avoids waiting on
  the exchange, such as protocol gap-fill, or a local component that keeps a
  current book per symbol and serves it as a snapshot to a restarted consumer
  (a restarted book process, a journal reopened mid-connection).
- A book desync does not itself force a reconnect yet: the book stays desynced
  until its connection reconnects for another reason. A thread-safe reconnect
  request is follow-up work.
- Reconnects must be rate-limited by the clients, because each costs a signed
  REST call or a Logon (`docs/modules/kraken-client.md`,
  `docs/modules/deribit-fix.md`).
- Not constrained here: how each book detects its own integrity issues
  (`docs/modules/order-book.md`).
