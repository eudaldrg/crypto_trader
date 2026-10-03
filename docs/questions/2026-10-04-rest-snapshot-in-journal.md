---
status: open
component: feed-handler, journal
blocking: true (for the Bitso client, #18)
raised-by: second ADR review, 2026-10-04 (decisions/0009 and #18)
assumed: the journal holds only what a socket received; REST responses are not journaled
---

# How does a book snapshot that arrives over REST get into the journal?

`decisions/0009` journals inbound socket bytes only, and deliberately leaves REST
reference data (Kraken `AssetPairs`) out: a replay is given each instrument's scale
separately. That works because Kraken and Deribit send their book snapshot on the
socket, so the journal alone rebuilds the book.

Bitso does not (`exchanges/bitso.md`). The stream carries only diffs; the full
book comes from a REST call (`/api/v3/order_book?aggregate=false`), spliced onto
the buffered stream by `sequence`. A journal of the socket alone cannot be
replayed into a book, and the splice point is the part most worth reproducing.

To decide, before the Bitso client is built:

- **Where the snapshot goes.** Its own record type in the same `connect_id`
  file, or a separate file beside it.
- **How it is ordered against the stream.** By capture sequence (when it was
  received) and by Bitso `sequence` (where it splices); a reader needs both.
- **Whether this widens 0009's rule** from "what the socket received" to "what
  the book was built from, inbound only", and what that means for other REST
  data (it should still not pull in `AssetPairs` by accident).
- **Format version.** A new record type is a format change under 0009's change
  policy.
