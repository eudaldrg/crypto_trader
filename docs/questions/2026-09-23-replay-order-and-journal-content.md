---
status: open
component: feed-handler, order-book
blocking: false
raised-by: ADR cleanup, 2026-09-23 (decisions/0004's "replay resync is underspecified", and the owner)
assumed: the journal keeps raw inbound wire bytes per connection (decisions/0009)
---

# How does replay reproduce the order live books applied updates in?

Live, one book thread polls each connection's ring in turn (`decisions/0010`), so
the order it applies two connections' updates depends on timing and is recorded
nowhere. Each journal file has its own capture sequence. `journal_replay` reads one
exchange's journals. Nothing yet tests that replay applies book updates in the
order live did, and no multi-exchange replay onto one book thread exists.

The owner does not consider the journal format settled until this is answered,
ideally with byte-identical update order between live and replay.

Options seen so far:

- Record the book thread's consumption order (a small ordering log of
  `(connection, capture_sequence)` pairs) beside the raw journals.
- Journal normalized book events after the adapter, as the replay source.
- Merge the raw journals by timestamp (#6) and accept that it approximates, not
  reproduces, live order.

Related: #5 (format v2, rotation, manifest), #6 (k-way merge replay).
