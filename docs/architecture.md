# Architecture

How the pieces fit and the design choices that are cheap to change. The
expensive-to-reverse decisions are ADRs in `decisions/`; open questions are one
file each in `docs/questions/`; how each component works is in `docs/modules/`.
This file is meant to change as the project learns.

## Execution modes

The pipeline is built for three modes, of which only the first exists:

1. **LiveTrading**: real exchange connections feed order books that strategies
   read. These books apply the exchange's diffs and never match; self-match
   prevention, if any, is a strategy concern.
2. **Replay**: a captured session replayed deterministically for debugging: if N
   exchanges fed M strategies live, each strategy gets the same callbacks in the
   same order on replay.
3. **Simulation**: a fake exchange replays captures in best-approximated real
   order, keeps *matching* books, and takes orders from the same order-entry hook
   that sends real orders live and is a no-op (or a digest) in Replay.

Strategy-facing code is meant to be identical across the three; only the code
nearest the exchange differs (`decisions/0004`).

## Components

```
exchange ─► client (connection thread) ─► CaptureSession ─┬─► journal ring ─► journal thread ─► file
                                                           └─► book ring ─► book thread ─► BookAdapter ─► books
journal file ─► journal_replay ─► BookAdapter ─► books          (inline, no ring)
```

- `src/feed_handler/`: clients, capture session, journal, config, the `feed_handler`
  binary. [feed-handler](modules/feed-handler.md), [kraken-client](modules/kraken-client.md),
  [deribit-fix](modules/deribit-fix.md).
- `src/order_book/`: the golden books. [order-book](modules/order-book.md).
- `src/book_adapter/`: frames to books, live and replay (`decisions/0008`).

## Capture process and configuration

- **One process runs every connection**, across exchanges. The two per-exchange
  binaries it replaced were about 60% identical, and the epoll-group target
  (`decisions/0010`) and cross-feed consumers both want the connections in one
  address space.
- **Configuration is a TOML file**, one `[[connections]]` entry per socket and
  journal. TOML because its array-of-tables is exactly "a list of connections",
  its scalars are unambiguous and it allows comments. Schema:
  [feed-handler](modules/feed-handler.md#schema).
- **Capture scope target: about the top five symbols per exchange**, one
  connection each, raw and uncompressed, about 6 GiB a day. Measurements and the
  alternatives weighed are in
  `docs/investigations/2026-09-20-capture-volume.md`. Re-measure once it runs;
  per-symbol volume varies widely.
