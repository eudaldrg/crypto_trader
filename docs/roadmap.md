# Roadmap

What gets built, in what order, and what each step lets the owner do that they
could not do before. The how is in `docs/architecture.md`, the expensive
decisions in `decisions/`, one milestone's detailed design in
`docs/design/<milestone>.md`.

**Status: draft, 2026-10-04.** The milestones and their order are the owner's,
from the first roadmap session, as is the v0 line after M5. M1's exit test is
confirmed; the others are proposals.

## Goal

A from-scratch trading engine for crypto markets, built as a portfolio piece:
real exchange data in, books, a strategy, orders out, with every step
reproducible. Made correct first, then fast.

## The v0 line

**v0 = the closed loop: capture, books, replay, a strategy, order entry and a
simulated exchange (M1 to M5).** That is the smallest thing that is a trading
engine. Everything after v0 makes it faster, wider or richer: M6 (a real test
server) is the first such step, worth doing for the portfolio, not needed for
v0.

- **M1 is the first stopping point** worth showing: "I have something working".
- **Performance work waits for v0.** Fast books, profiling-driven rewrites and
  latency targets come after the loop is closed, measured against the golden
  book (`decisions/0013`). Correct first, then fast.

## Non-goals (proposed, confirm)

- Trading real money. Orders go to the simulator or to exchange test servers.
- Making money. Strategies exist to exercise the engine, not to have an edge.
- **UI as a product.** The product is the engine. Visualization (books over
  time, time series, what each component did at a given moment) is a debugging
  and analysis tool. It will be needed and may grow complex, but it is built
  only as far as debugging the engine needs, and never drives the engine's
  design: debugging tools may take shortcuts (read raw journals, per-exchange
  views, side channels) that engine code may not.

## How this roadmap changes

Things get discovered while building; the roadmap is expected to change, under
these rules:

- **Only the next milestone is detailed.** Later ones stay a few lines, so
  changing them costs little. A milestone gets its design doc
  (`docs/design/<milestone>.md`) just before it starts.
- **A finished milestone is never reopened.** New work it turns out to need
  becomes part of the current milestone or a new one inserted in order.
  Renumbering is fine while the roadmap is a draft.
- **The exit test is the scope gate.** Discovered work joins the current
  milestone only if its exit test cannot pass without it; otherwise it goes to
  a later milestone or the backlog.
- **A changed decision is superseded, not edited.** A new ADR replaces the old
  one, which is marked `superseded by NNNN` and otherwise left as it was.
  A changed design-doc section says what changed and why in the same commit.
- **Unknowns get a question file**, an assumption, and work carries on
  (`docs/questions/`). A cheap probe (`experiments/`) settles an unknown before
  a milestone is designed around it.

## M1: Capture, golden book, replay

- **Goal:** record live market data from Kraken and Deribit, rebuild the order
  books from it live, and replay a recorded session through the same books
  offline.
- **Scope:** the `feed_handler` capture binary, the journal, the golden book,
  the book adapter, `journal_replay` (#14). Most of it exists.
- **Exit test:** capture an hour of the configured symbols; replay the journal
  twice; both runs end with identical books and zero checksum or integrity
  failures, and the replayed books match the books the live run built.
- **Depends on:** nothing.
- **Design needed:** how live and replayed books are compared (a book digest
  or dump); how much of #17 (journal failure containment)
  belongs here.
- **Design status:** not started (most components exist; the milestone was never
  written down).

## M2: Normalized market data

- **Goal:** everything above the book adapter sees one normalized model of
  market data, not each exchange's messages. The milestone is the
  normalization; a second L3 exchange is how it gets proven.
- **Why a new exchange:** with one L3 exchange, "generic L3" is likely Kraken's
  message structure under another name. Bitso `diff-orders` differs where it
  matters (+1 sequence numbers, REST-snapshot recovery, status-based updates),
  so a model that serves both is generic by evidence, and a third exchange gets
  cheaper.
- **Scope:** the normalization boundary and the normalized model; Bitso capture,
  book and replay (#18) as its second L3 source.
- **Exit test (proposed):** M1's exit test passes for Bitso too; Kraken and
  Bitso feed the same generic `L3Policy`/`L3Update`, with every difference in an
  exchange policy; nothing above the adapter names an exchange's message type.
- **Depends on:** M1.
- **Design needed:** the normalized event fields (the boundary itself is
  `decisions/0014`); which thread runs the feed book; whether Deribit's L2
  maps onto the same model; how the REST snapshot is journaled
  (`docs/questions/2026-10-04-rest-snapshot-in-journal.md`); whether Bitso
  recovery is a reconnect (`decisions/0011`) or a REST resync.
- **Design status:** not started.

## M3: A strategy reads the books

- **Goal:** a toy strategy receives book updates through one interface, the
  same live and on replay, and gets the same callbacks in the same order on
  replay.
- **Scope:** the strategy hook and runner (#10), the event stream from books to
  strategies, a toy strategy that only logs or computes signals.
- **Exit test (proposed):** the toy strategy's output on a replayed journal is
  byte-identical across two runs, and identical to its output in the live run
  that recorded the journal.
- **Depends on:** M2 (the normalized model the strategy reads).
- **Design needed:** the strategy interface; replay order across connections
  (`docs/questions/2026-09-23-replay-order-and-journal-content.md`, #6).
- **Design status:** not started.

## M4: Order entry and a simulated exchange

- **Goal:** orders go through one order-entry module to a simulated exchange
  that matches them against the captured market it replays.
- **Why together:** the order-entry module is the same whether it talks to an
  exchange gateway or to the simulator; only the destination differs. The
  simulator is the first destination, and the safest.
- **Scope:** the order-entry interface and module (#11), the simulated exchange:
  its matching book (`docs/questions/2026-09-23-matching-seam.md`), queueing our
  orders against the orders in the replayed market data, fills and order-state
  replies.
- **Exit test (proposed):** a scripted test client sends orders through the
  order-entry module to the simulator on a captured journal; fills are
  deterministic across runs and consistent with the replayed book.
- **Depends on:** M3 (the event stream and replay order).
- **Design needed:** the order-entry interface, shaped by a real exchange's
  order messages (Deribit FIX order entry, `exchanges/deribit.md`) even though
  the simulator is its first destination; the order state machine; matching
  algorithm and per-exchange quirks; what fills are realistic enough (queue
  position, latency model); risk checks before an order leaves.
- **Design status:** not started.

## M5: A strategy trades in simulation

- **Goal:** the toy strategy from M3 places, amends and cancels orders through
  the order-entry module, against the simulator. The loop is closed: v0.
- **Scope:** the strategy side of order entry (own-order tracking, the
  order-entry hook a strategy calls), the toy strategy extended to trade.
- **Exit test (proposed):** the toy strategy trades on a captured journal in
  simulation; two runs produce identical orders, fills and positions.
- **Depends on:** M4.
- **Design needed:** own-order tracking (#10's side-table idea); how a strategy
  learns of fills and rejects.
- **Design status:** not started.

## M6: Order entry on a test server (after v0)

- **Goal:** the same strategy binary trades on Deribit's FIX testnet, the
  order-entry module now talking to a real exchange.
- **Scope:** the Deribit order-entry gateway (#11), reference data (#9).
- **Exit test (proposed):** a session places, amends and cancels orders on the
  Deribit testnet; every order's final state in the engine matches the
  exchange's.
- **Depends on:** M5.
- **Design needed:** the Deribit FIX order-entry session; reconciling engine
  order state with the exchange after a disconnect.
- **Design status:** not started.

## Later / backlog

Real, not scheduled. Ordered roughly by how soon they matter after v0.

- **Fast books**: exchange-specific, cache-friendly books checked against the
  golden book (`decisions/0006`, `decisions/0013`). The first post-v0 milestone.
- Profiling and latency targets; simdjson for the Kraken book path.
- Visualization for debugging and analysis: books over time, time series,
  per-component state. Pulled in by whichever milestone first needs it.
- Journal rotation, format v2, manifest (#5).
- RX timestamping (#7), backpressure instrumentation (#8).
- Journal failure containment (#17), unless M1 needs it.
- Zero clang-tidy warnings, enforced (#19): hygiene, can land anytime.
- Strategies and the book in separate processes (shm ring, #10's deferred part).
- More realistic strategies.
- Deribit SBE multicast (`docs/architecture.md`, stretch goal).
