# 0011. Lost data is recovered by reconnecting and taking a fresh snapshot

- Status: proposed
- Date: 2026-09-23

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

Any loss ends the connection. The next connection gets a new `connect_id` and a
fresh snapshot, and everything downstream resets at that boundary. One story for
every exchange and every kind of loss. Hard: a reconnect costs a REST token call
(Kraken) and a Logon (Deribit), plus the time to the next snapshot.

### C. Keep applying and flag the book

No reconnect cost. Hard: a book that is wrong looks healthy.

## Decision

B. Detection is local and recovery is uniform:

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

- There is no gap-fill code. It is worth building only if real sessions gap often
  enough that reconnecting costs too much.
- A book desync does not itself force a reconnect yet: the book stays desynced
  until its connection reconnects for another reason. A thread-safe reconnect
  request is follow-up work.
- Reconnects must be rate-limited by the clients, because each costs a signed
  REST call or a Logon (`docs/modules/kraken-client.md`,
  `docs/modules/deribit-fix.md`).
- Not constrained here: how each book detects its own integrity issues
  (`docs/modules/order-book.md`).
