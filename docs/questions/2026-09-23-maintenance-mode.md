---
status: open
component: feed-handler, order entry, strategies
blocking: false
raised-by: ADR cleanup, 2026-09-23 (decisions/0012)
assumed: nothing is built; any fatal still stops the process
---

# What does the process-wide maintenance mode do, and when does it start?

`decisions/0012` commits only to the rule: shared-resource exhaustion (disk,
memory) escalates to a degraded maintenance mode rather than a stop. Low
priority; it needs order entry (#11) and strategies (#10) to exist.

The owner's current intent, moved out of 0012 on 2026-10-04 because nothing it
governs exists yet: market-data journaling is shed first, before the disk is
full; order and execution logs and saving portfolio state keep a reserve;
trading is restricted to reducing risk; an operator is alerted.

To decide:

- **Trigger.** A free-disk watermark (the owner's example: stop market-data
  journaling at about 5% free), checked by what and how often? Memory?
- **What is protected, and how.** A reserved budget for order and execution
  logs and for saving portfolio state at the end of a run, so they still fit
  after market-data journaling has filled what it may.
- **Trading behavior.** Hedge-only / reduce-only, never adding position; who
  enforces it (order entry, or each strategy)?
- **Alerting.** Mail or another channel to whoever can free space, and how the
  same alert serves connection-level failures (a stopped journal, a connection
  that keeps failing to reconnect).
