# 0006. Order books vary by compile-time policies; exchange quirks live in exchange policies

- Status: proposed
- Date: 2026-09-23

## Context

The order book is the stated hot path (target: under 100 ns per update on the
eventual fast books), and it varies along two independent axes:

- **Granularity:** L1 (best bid/ask), L2 (aggregated levels), L3 (individual
  orders by `order_id`). L2 alone has two wire shapes: Deribit's WS `book` with
  `change_id` sequencing, and Deribit FIX with none.
- **Matching:** live and replay books apply the exchange's diffs as given; a
  simulation book must also match orders.

On top of that, each exchange has quirks no other shares: Kraken's `level3`
checksum, its client-side depth truncation, its add ordering by timestamp.
Whatever mechanism is chosen spreads through every book, every listener and
every test.

## Options

### A. Virtual interfaces per axis

Familiar, swappable at runtime. Hard: a virtual call per update on the hot path,
and runtime swapping is never needed; a book's granularity and feed are fixed
when it is built.

### B. CRTP

Static dispatch through a derived-class downcast. Hard: needs a "self" the
engine calls back into, which this design does not have, and couples each book
to one base.

### C. One engine templated on concept-constrained policies

`OrderBook<GranularityPolicy, ListenerT, MatchingPolicy>`: each axis is a
template parameter constrained by a concept naming the minimal shape it must
expose. Easy: no vtable, any structurally conforming type fits (including a
plain test fake, which is the project's mocking strategy for books), and a
wrong signature is a compile error. Hard: template error messages, and
everything is header code.

## Decision

C. The engine owns readiness, the apply entry points and listener
notification; a granularity policy applies updates and returns a
policy-defined change set that the engine turns into listener calls. The engine
never branches on which policy it holds.

Exchange-specific behavior goes in an exchange-specific policy that composes a
generic one (`KrakenL3Policy` wraps `L3Policy`), never in the generic engine or
policies. A policy with settings (a subscribed depth) is constructed configured
and passed in by value.

## Consequences

- A listener must implement `OnTopOfBookChanged`; that one hook is enforced at
  compile time so a typo cannot silently skip it. Every other hook is optional
  and detected with `requires`.
- A misuse the type system can catch is caught there: the sequenced L2 book
  cannot be applied without its `change_id` and the unsequenced one cannot be
  given one (`docs/modules/order-book.md`).
- The matching axis is a placeholder concept with one implementation
  (`NoMatchingPolicy`); a real matching policy will need more of the concept
  and a second entry point (`docs/questions/2026-09-23-matching-seam.md`).
- Control-plane interfaces called a handful of times per process (for example
  `CaptureConnection`) may stay virtual; this decision is about the per-update
  path.
- How the books are layered against the capture code is `decisions/0008`; how
  lost data is handled is `decisions/0011`; that the simple book is the oracle
  for the fast ones is `decisions/0013`.
