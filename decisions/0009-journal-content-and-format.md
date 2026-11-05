# 0009. The journal records inbound wire bytes, in a versioned format

- Status: proposed
- Date: 2026-09-23
- Revisit when: replay is designed to reproduce the order in which live books
  applied updates across connections (#5, #6). That may call for journaling
  normalized book events, instead of or besides wire bytes. Also when Bitso is
  built (#18): its snapshot arrives over REST, not the socket, and replay
  needs it (`docs/questions/2026-10-04-rest-snapshot-in-journal.md`).

## Context

Captures are what Replay, profiling and the order-book tests run on, so what a
journal holds decides what can ever be reproduced. A process can crash mid-write.
Kraken's `subscribe` carries the WebSocket auth token in the message body
(`exchanges/kraken.md`), and journals are archived. The captures kept so far are
minutes long, so nothing stored yet is worth a compatibility promise.

## Options

### A. Inbound wire bytes, one file per connection and `connect_id`

What each socket received, verbatim, in arrival order. Easy: a parser or book bug
found later is fixed and the capture re-read; capture works with books off.
Hard: replay has to re-parse; each file has its own order, so the order in which
one book thread interleaved several connections live is not recorded.

### B. Normalized book events, after the adapter

What the books applied, in the order they applied it, across connections. Easy:
byte-identical replay of the book-update order. Hard: a parser or adapter bug is
baked into the capture for good, and the format follows the book's event schema,
which will change.

### C. Both

A as the source of truth, plus an ordering or event log. Costs a second writer
and a second format.

### D. One file per symbol

Rejected: splitting needs parsing before journaling, and destroys the one true
arrival order of a multi-symbol connection.

## Decision

A for now, because it is the only one of A-C that survives a parser bug, and the
cross-connection ordering it lacks is not needed until replay determinism is
designed (the revisit trigger).

Whatever the content, these hold:

- **Inbound only.** Outbound traffic is never journaled, so a credential cannot
  end up in an archived file. Enforced structurally: the journal writer has no
  outbound path.
- **Everything inbound**, control and heartbeat messages included, and written
  before the client classifies the message.
- **Self-describing and crash-tolerant.** A file header with magic and version;
  per record a type, a capture sequence number, a monotonic timestamp and a
  checksum; a wall-clock anchor per file. A reader treats the first invalid
  record as the end of valid data.
- **Change policy.** A format change bumps the version. Until a capture worth
  keeping exists, no old version needs a reader; once one does, a change keeps a
  reader for the previous version or migrates the files.

## Consequences

- The byte layout is defined once, in `src/feed_handler/journal_format.h`.
- A reconnect is an explicit record, so a reader never infers it.
- Instrument reference data fetched over REST at startup (Kraken `AssetPairs`)
  is not journaled; a replay is given each instrument's scale separately.
- Replaying several connections in their live interleaving is not possible from
  this format alone.
- Not constrained here: file rotation and a manifest (#5), or compression.
