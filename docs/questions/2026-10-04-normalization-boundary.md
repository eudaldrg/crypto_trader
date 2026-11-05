---
status: answered by decisions/0014; event fields go to the M2 design doc
component: book-adapter, strategy
blocking: true (designing M2 in docs/roadmap.md)
raised-by: roadmap session, 2026-10-04
assumed: raw wire bytes up to the book adapter (decisions/0004, 0009); books expose exchange-agnostic listener hooks
---

# Where is exchange data normalized?

The owner's concern: if every exchange keeps its own message types all the way
through, every component above the feed (strategy, simulation, risk) needs one
version per exchange, and the code is duplicated everywhere.

What is already true, which narrows the question:

- "Keep the data as is" (`decisions/0004`, `decisions/0009`) applies to the
  capture edge: the journal and the sink seam carry raw wire bytes. It does not
  say strategies see wire messages.
- The book adapter already normalizes. Deribit FIX becomes the same `L2Update`
  as any L2 feed; Kraken `level3` goes through `KrakenL3Policy`, which wraps the
  generic `L3Policy`. The books' listener hooks (`OnTopOfBookChanged`,
  `OnLevelChanged`, `OnOrderAdded`, ...) are exchange-agnostic.
- #10's direction is a stream of normalized book deltas to strategies.

So the question is not whether to normalize but where the boundary sits and
what crosses it:

- Strategies see books and normalized events (book changes, trades, own-order
  updates) only, never wire messages?
- What does a normalized event lose that a strategy might need (exchange
  timestamps, Kraken's per-order timestamps, Deribit's FIX fields)? Carried as
  optional fields, or dropped?
- Order entry (M3) needs the reverse direction: one order interface, an
  exchange-specific encoder behind it.
- Simulation (M4) needs exchange quirks in matching; does that follow
  `decisions/0006` (generic matching policy, exchange policies composing it)?

Probably an ADR once answered: it is the contract every component above the
book depends on.

## Owner's position so far (2026-10-04)

Leaning towards a normalized core (option B in the session's framing: one
normalized event type, exchange-specific details reachable only on purpose):

- **Strategy books** (non-matching) update from fully normalized data. A book
  is generic: orders added, modified, removed; levels created and deleted; best
  bid and ask. Exchange variations (implied volume, shown-volume limits, L2
  depth limits) can be expressed generically.
- **Simulator** holds its own per-exchange implementations, matching books
  included, which may keep exchange-specific state internally. It publishes
  market data to the rest of the engine like a real feed.
- **Generic strategies never see exchange-specific data.** A strategy written
  for one exchange may later get explicit per-exchange events that bypass the
  book (used in past work to identify participants on one exchange). Not needed
  for v0; designed when a strategy needs it.
- **Visualization is not architecture.** Debugging tools may take shortcuts:
  read raw journals, have per-exchange views, use side channels outside the
  regular run.
- **When a field exists only on some exchanges**, it is either left empty for the
  others or several exchange fields serving one purpose are merged into one
  generic field. Decided per field, when a consumer needs it.
- Likely needed in the core: a shared order-id definition, and an ordering for
  events (sequence numbers where the exchange has them, or assigned by the
  engine).

Still to settle: where exchange-specific integrity checks (Kraken's checksum,
Bitso's and Deribit's sequence numbers) run if strategy books only see
normalized data, and what a normalized event carries for ordering and identity.
