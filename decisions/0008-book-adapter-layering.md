# 0008. Books and capture stay independent libraries; a third one joins them

- Status: proposed
- Date: 2026-09-23

## Context

The order books (`src/order_book/`) and the capture code (`src/feed_handler/`)
were built separately, and something has to turn a connection's frames into
book updates: parse each exchange's wire format, route by symbol, reset on a new
`connect_id`. Where that code lives decides which library depends on which, and
dependency directions spread: every later component (strategies, simulation, a
fast book) will be placed relative to them.

## Options

### A. Inside `order_book`

Books would come with their own feed parsing. Hard: `order_book` gains a
`feed_handler` dependency (the Deribit FIX normalizer reads `feed_handler`'s FIX
parser), so every book test and every future book consumer links the capture
code.

### B. Inside `feed_handler`

The capture code would build books. Hard: a capture that only journals would
link the book code, and a book bug would live inside the component whose job is
to never lose data.

### C. A third library, `book_adapter`, depending on both

`order_book` and `feed_handler` stay independent of each other; only
`book_adapter` and the binaries that want books link both. Costs generic hooks
in `feed_handler` so an outside library can attach to a connection.

## Decision

C. `order_book` never depends on `feed_handler`, `feed_handler` never depends on
anything downstream of capture, and code that needs both lives in
`book_adapter` (or, later, its own library on the same terms).

## Consequences

- `feed_handler` offers generic hooks instead of knowing about books:
  `CaptureConnection::AddSink` and the `BeforeStart` callback of
  `CaptureSet::StartAll`. The capture binary is the one target that links both
  libraries and plugs books in through them.
- Wire parsers that read `feed_handler` types (the Deribit FIX book normalizer)
  live in `book_adapter`; parsers that need nothing from it (`kraken_l3_wire.h`)
  can live in `order_book`.
- The adapter itself has no threads or rings, so live (behind a ring,
  `decisions/0010`) and replay (inline, from the journal reader) run the same
  code.
- Not constrained: where strategies live (#10).
- How the adapter behaves (lazy books, stale handling, drop and desync
  accounting, configuration) is `docs/modules/order-book.md` and
  `docs/modules/feed-handler.md`.
