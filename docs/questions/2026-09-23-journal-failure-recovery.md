---
status: open
component: feed-handler
blocking: false
raised-by: ADR cleanup, 2026-09-23 (decisions/0012)
assumed: a failed journal stays stopped for that connection until a restart, with an alert
---

# Can a connection's journal restart automatically after it fails?

Under `decisions/0012` a journal failure (ring overflow, failed write, file that
cannot be opened) stops journaling for that connection and alerts; the socket
and books carry on. Whether it should then restart on its own is deliberately
undecided until a real failure shows what kind it was. The owner expects most
to be disk problems that need a person anyway.

What a restart would have to solve:

- **Transient vs. permanent.** Retry opening a file (or a new one) while frames
  queue in the ring, and give up for good only when the ring overflows?
- **A journal reopened mid-connection has no snapshot.** The exchange sends one
  only at subscribe, so a new file would start with incrementals no reader can
  apply. Options: force a reconnect of that connection (costs the socket, gets
  a real snapshot, cheap on both current exchanges); or synthesize a snapshot
  from a local book, which is the local-recovery target in `decisions/0011`.
