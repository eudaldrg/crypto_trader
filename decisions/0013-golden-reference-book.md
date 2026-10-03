# 0013. A simple golden book is the oracle every fast book is checked against

- Status: proposed
- Date: 2026-09-23

## Context

The fast books this project exists to build will be exchange-specific, cache-
conscious and hard to read. Exchange feeds have quirks that only show up against
real data: Kraken's depth truncation and add ordering were invisible to
hand-written tests and surfaced minutes into a real capture
(`exchanges/kraken.md`), and a Deribit capture caught a wire-shape assumption
the first design had wrong. A fast book needs something trustworthy to be
compared against, and so does each new feature.

## Options

### A. Build the fast books directly, tested against hand-written expectations

Least code. Hard: the expectations are only as good as the author's reading of
the docs, which is exactly what real captures proved wrong.

### B. A simple golden book first, as the oracle

A deliberately simple book (`std::map` levels, no clever storage), validated
against real captures and the exchanges' own documented examples. Every fast book
is checked against it on the same inputs. Costs a second implementation of
every feature.

### C. Validate only against exchange checksums

Free where it exists. Hard: only Kraken has one, and a checksum covers the top
of the book, not queue order deeper down or L1/L2 books.

## Decision

B, as a change policy:

- A fast book is correct when it agrees with the golden book on the same
  inputs, real captures included.
- A new feature (hidden or iceberg volume, implied volume, pro-rata matching, a
  new exchange quirk) lands in the golden book first, then in fast books.
- The golden book stays simple.

## Consequences

- Every book feature is written twice.
- Real captured sessions are test fixtures, committed compressed
  (`docs/modules/order-book.md`), and a capture that disagrees with the golden
  book is a finding about the book or about the exchange, recorded in
  `exchanges/`.
- The golden book's read surface (`BookView`) is what comparisons and the Kraken
  checksum read, never a policy's internals.
- Not constrained: how the fast books are built, or whether they share the
  engine of `decisions/0006`.
