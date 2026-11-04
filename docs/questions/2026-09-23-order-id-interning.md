---
status: open
component: order-book
blocking: false
raised-by: ADR cleanup, 2026-09-23 (was a deferral in decisions/0008)
assumed: Kraken order ids are mapped with HashOrderId, a 64-bit hash; collisions accepted
---

# Should order ids be interned into dense ids, and how are they evicted?

A 64-bit hash can in principle map two live orders to one id; the odds at this
scale were estimated around 1e-9 and never measured. Interning into dense ids
removes that and gives the fast books small integer keys, but a table that is
never evicted grows without bound on a long live run.

Decide together with the fast books (`decisions/0013`): what evicts an entry
(the order's delete, a book reset on `connect_id`), whether ids are per book or
per connection, and whether the golden book interns too or keeps the hash.
