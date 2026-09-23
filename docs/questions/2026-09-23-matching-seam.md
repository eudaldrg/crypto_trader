---
status: open
component: order-book
blocking: false
raised-by: ADR cleanup, 2026-09-23 (was the "Matching axis" section of decisions/0006)
assumed: only NoMatchingPolicy exists; MatchingPolicyConcept requires MatchingEnabled() alone
---

# What does a matching-capable book need from the engine?

Simulation mode needs books that match: one for "our" orders and an exchange
book simulating the other participants, so a strategy's orders fill against
resting liquidity. The engine's `MatchingPolicy` parameter is a placeholder: its
concept requires only `MatchingEnabled()`.

To design, once Simulation mode or order entry (#11) is on the roadmap:

- a second entry point (`SubmitOrder` or similar) next to the feed's apply calls;
- trade and fill notifications the listener concepts do not model yet;
- the matching algorithm (price-time priority, self-match handling), which is
  shared with the simulated exchange book;
- whether it lands in the golden book first (`decisions/0013` says yes).
