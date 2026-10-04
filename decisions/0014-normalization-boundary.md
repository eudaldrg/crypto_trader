# 0014. Exchange data is normalized by a validating feed book per connection

- Status: proposed
- Date: 2026-10-04
- Settled at: M2 (`docs/roadmap.md`)

## Context

Every component above the feed (strategies, risk, the simulator's consumers)
needs market data. If each saw each exchange's own messages, each would need
one implementation per exchange, and adding an exchange would touch all of
them.

Normalizing is not free of exchange knowledge, though. Checking that a feed is
correct needs exchange-specific rules applied to exchange-specific book state:
Kraken's checksum is a CRC over its top 10 levels after each message; Kraken's
depth-limited subscription sends a delete when an order merely leaves the
visible depth; Bitso and Deribit have sequence numbers with their own gap
rules. Those checks decide when a connection desyncs (`decisions/0011`).

The journal and the sink seam stay raw (`decisions/0004`, `decisions/0009`);
this decision is about what is downstream of them.

## Options

### A. Exchange types all the way up

Each exchange keeps its own message and book types; components above are
templates over a shared concept. No translation step, every exchange detail
reachable. Hard: every component above the feed is a template, events from two
exchanges cannot share one queue or one replay order, and reversing it means
rewriting every consumer.

### B. Translate the wire to normalized updates, then generic books validate

A stateless translator per exchange, generic books everywhere. Hard: the
integrity checks above need exchange-specific book state, so they either leak
into the generic books or are lost.

### C. A validating feed book per connection is the boundary

One feed book per connection applies the raw updates through exchange policies
(`decisions/0006`), runs every exchange-specific check and handles desync. Its
output is a stream of normalized events. Everything downstream, including the
books strategies keep, is exchange-agnostic. Hard: the normalized events must
carry what the checks learned (validity, why an order left), and fields only
some exchanges have need a rule.

## Decision

C, because it is the only option where the integrity checks keep their
exchange-specific state and nothing downstream needs any:

- Exchange-specific code lives only in the feed handler and the feed book's
  exchange policies.
- Downstream components consume only normalized events, which carry enough to
  rebuild a book without knowing the exchange, including whether the source
  book is currently trustworthy.
- A field only some exchanges provide is either optional in the normalized
  event or merged with fields that serve the same purpose on other exchanges;
  it is never a reason for a consumer to know the exchange.

## Consequences

- Two book roles: the feed book (one per connection, validating) and consumer
  books (generic, no feed checks). Whether consumer books check their own
  invariants (crossed book) is left to the strategy design.
- The simulator publishes the same normalized events to its consumers; how it
  keeps exchange-specific state internally (its matching books) is its own.
- A strategy that needs exchange-specific data, such as participant behavior on
  one exchange, gets an explicit opt-in channel when one is needed. Not
  designed yet.
- Debugging and visualization tools are not bound by this: they may read raw
  journals.
- Not constrained here: the event fields (M2's design doc), which thread runs
  the feed book (`decisions/0010`), and whether the feed book is the golden book
  or a fast one (`decisions/0013`).

## Evidence

- `docs/questions/2026-10-04-normalization-boundary.md`
- `exchanges/kraken.md`, `exchanges/bitso.md`, `exchanges/deribit.md`
