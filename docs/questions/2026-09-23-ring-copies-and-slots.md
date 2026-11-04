---
status: open
component: feed-handler, order-book
blocking: false
raised-by: ADR cleanup, 2026-09-23 (was in decisions/0004 and decisions/0008)
assumed: each ring push allocates a std::vector and copies the payload; journal and books each get their own copy
---

# Are the per-frame allocation and the second copy worth removing?

The connection thread allocates and copies each frame once per ring
(`decisions/0010`): one for the journal ring, one for the book ring. Two ways to
cut that, each only if profiling asks for it:

- **Preallocated slots**: a ring of fixed-size buffers instead of a vector per
  frame, removing the allocation on the connection thread. Needs a policy for a
  frame larger than a slot.
- **One ring, two read cursors**: one copy shared by the journal and book
  consumers. The ring library in use (moodycamel `readerwriterqueue`) does not
  offer it, so it means a custom ring.

Evidence to gather first: `journal_replay` on a long capture, and the connection
thread's time per frame live.
