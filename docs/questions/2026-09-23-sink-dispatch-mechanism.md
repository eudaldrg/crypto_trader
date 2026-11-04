---
status: open
component: feed-handler
blocking: false
raised-by: ADR cleanup, 2026-09-23 (was an open point in decisions/0004)
assumed: MessageSink stays a plain virtual interface
---

# Should `MessageSink` dispatch be virtual or a compile-time policy?

`MessageSink::OnFrame` is a virtual call per frame per sink. With the journal and
books now behind rings (`decisions/0010`), the sinks on the connection thread are
ring pushes, so the per-message hot path the book's <100 ns target is judged on
is on the book thread, not here.

Options: keep virtual; template the capture session on its sink set; or a small
fixed variant of known sinks.

Evidence to gather first: the cost of the virtual call against the copy and ring
push beside it, measured with a live or replayed load. Until then there is
nothing to decide on.
