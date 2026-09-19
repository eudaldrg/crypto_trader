# 0006. Order book design: compile-time polymorphism, golden reference first

## Context

The order book is the project's stated hot path (target: sub-100ns per MBO
insert/modify on the eventual fast implementation), so polymorphism across
its two natural axes has to be resolved at compile time, not through virtual
dispatch. The two axes:

- **Granularity**: L1 (best bid/ask only), L2 (aggregated price levels), L3
  (individual order-level add/modify/delete by `order_id`, matching
  Kraken's `level3` shape, with its per-message `checksum` for desync
  detection, per decisions/0001).
- **Matching**: a live/replay book only applies already-matched diffs from
  the exchange; a simulation book additionally needs to run a matching
  engine, both for "our own book" and for an "exchange book" simulating
  other participants so a strategy's own orders can be matched against
  resting liquidity.

L2 is two distinct wire shapes sharing one granularity, not one shape:
Deribit's WS `book` channel uses `change_id`/`prev_change_id` sequencing,
while Deribit's FIX market data uses session sequence numbers and an
`MDUpdateAction` (New/Change/Delete) per entry (decisions/0001). This
design's `L2Update` models the WS shape; normalizing FIX into it is the
feed handler's job, not the book's.

The project builds at C++23 (`CMakeLists.txt:4`).

The approach here was reviewed by an Opus-level design pass before
implementation started; several of the decisions below (the readiness
state machine, the message/batch boundary, the error taxonomy, the
checksum's placement, the mandatory/optional listener hook split) exist
specifically because that review found the first draft under-specified or,
in one case, structurally unable to deliver what it promised.

## Decision

### Golden reference first

The first implementation is deliberately simple: `std::map`/
`std::unordered_map` per price level, not Abseil/ankerl. It supports every
update type across L1/L2/L3 (matching mode is a seam, see below) and is
the correctness oracle later fast, exchange-specific implementations get
validated against. Abseil/ankerl wiring is real added build-infra work
that only pays for itself once an optimization pass actually needs it —
deferred until then. A `LevelMap` alias
(`template<class K, class V, class Cmp> using LevelMap = std::map<K, V, Cmp>`,
`Cmp = std::greater<>` for bids, `std::less<>` for asks) keeps that later
swap a typedef change rather than a rewrite.

Any new exotic feature (hidden/iceberg volume, implied volume, pro-rata
matching, etc.) gets implemented in the golden book first, before a fast
version tackles it.

### Polymorphism mechanism: concepts, not CRTP

Both axes are resolved with C++20 concepts and `if constexpr (requires
{...})`, not CRTP. CRTP's static self-type/downcast pattern isn't actually
needed here — there's no case where the engine needs to call back into a
statically-typed "self." What's needed is *policy-based* parametric
polymorphism: a `GranularityPolicy` and a `MatchingPolicy` supplied as
independent template parameters, each constrained by a concept describing
the minimal shape it must expose. Any structurally-conforming type —
including a plain GoogleTest fake struct — satisfies a concept with no
shared base class, no vtable, and no mocking framework. This is also the
project's mocking strategy for order-book tests: a fake listener or fake
matching policy is just a struct with the right method signatures.

### Shared engine and the engine/policy contract

One template engine, not three independent L1/L2/L3 classes:

```
template<class GranularityPolicy, class ListenerT,
         class MatchingPolicy = NoMatchingPolicy>
class OrderBook;
```

The engine owns: a reference to `ListenerT` (caller-owned, must outlive
the book — see Thread-safety below), a generic `notify(...)` helper, the
readiness state machine, the `apply()`/`apply_snapshot()`/`apply_batch()`
entry points, and the `MatchingPolicy` seam. `GranularityPolicy::apply(...)`
/`apply_snapshot(...)` return a policy-defined `ChangeSet` value that the
engine translates into listener notifications. Granularity-specific
behavior lives in the `ChangeSet` shape and the policy itself, not as an
engine-side type-switch keyed on which policy it holds — the alternative
(branching inside the engine on which concrete policy is active) is worse
than a vtable, because it's an invisible, unenforced coupling instead of
an explicit one.

### Listener dispatch: mandatory vs. optional hooks

Every granularity's listener concept requires `on_top_of_book_changed` as
a hard, compile-time-enforced minimum — a missing or wrong-signature
implementation is a compile error, not a silently-skipped call. Additional
hooks are genuinely optional and detected via
`if constexpr (requires { listener.on_x(...); })`:
`on_level_changed`/`on_level_deleted` and the shared
`on_integrity_check_failed(IntegrityIssue)` for L2/L3, and
`on_order_added`/`on_order_modified`/`on_order_deleted` for L3 only.

Making only the baseline hook mandatory (rather than leaving every hook
duck-typed) closes a real footgun: a listener whose hook is silently never
called because of a typo'd name or a mismatched signature is a bug, not a
feature of "optional" dispatch.

### Query surface and the `BookView` oracle

All granularities expose `is_ready()` and `best(Side) -> std::optional<...>`.
L3 additionally exposes `Levels(Side)` (per-level order ids, unbounded, for
tooling rather than production logic — see below) and
`OrderQuantity(OrderId) -> std::optional<Quantity>`; there is no general
`depth(Side, n)` or `order(OrderId)` returning a full order record on either
L2 or L3 — an earlier draft of this ADR described both before either was
actually implemented. A minimal, granularity-generic `BookView` (dump-able,
equality-comparable) is the golden book's actual oracle surface — what the
Kraken checksum function and the captured-session replay tests read from,
rather than reaching into a policy's internal storage.

### Snapshot and readiness

An explicit `Uninitialized -> Ready -> Desynced` state machine.
`apply_snapshot(...)` is a distinct entry point that clears and
repopulates the book; `apply()`/`apply_batch()` on a not-`Ready` book is
defined (rejected/no-op), never undefined behavior. This exists because
decisions/0001 states, of Kraken specifically, that book "readiness"
reduces to "have I received a snapshot since my last (re)connect" — a
book with no readiness concept at all would contradict that directly.

### Message/batch boundary

`apply_batch(std::span<const Update>, MessageMeta)` validates sequencing
(Deribit `change_id`) or checksum (Kraken) once per message, not once per
individual update, since a real feed message can carry several updates
under one `checksum`/`change_id`. Top-of-book notification coalesces to
once per batch; individual order/level events still fire once per update
within it. Applying updates one at a time would make the checksum
uncomputable mid-message and would expose listeners to top-of-book states
that never actually existed on the exchange.

### Error taxonomy

Three tiers, applied consistently across L2 and L3:

1. **Feed-integrity anomalies** (`Gap`, `ChecksumMismatch`, `CrossedBook`,
   `UnknownOrder`, `UnknownLevel`) — reported through one shared
   `on_integrity_check_failed(IntegrityIssue)` listener hook, *and* the
   book transitions to `Desynced`. A genuine gap means the book's state is
   unknown, not "probably still fine" — it does not silently keep
   applying updates.
2. **API misuse** (duplicate add, negative quantity) — `assert()`,
   compiled out in Release via `NDEBUG`, active in Debug and in tests.
   Only partially implemented so far: `L2Policy::ApplyOne` asserts on a
   `New` for a price already tracked. A duplicate L3 `Added` for a live
   `order_id` is instead handled as a defined remove-then-re-add (see
   "Depth-limited books" below — this is the normal, expected shape of a
   re-entering order, not misuse), and `L3Policy` asserts that an order
   it is about to remove still has a level to be removed from. Negative
   quantities are not yet guarded anywhere.
3. **Nothing is silently ignored.** A `Change` for an untracked L2 price and
   a duplicate `New` for a tracked one used to fall through to
   `insert_or_assign` unreported; both now report `UnknownLevel`/assert
   instead (see item 2).

`std::expected` (available at C++23) was considered as an alternative
result channel for `apply()`'s return value and rejected for now, to keep
a single reporting channel (listener notifications) instead of two
parallel ones. Worth revisiting if a pull-based differential-test harness
is ever built against this book.

### Thread-safety and ownership

Single-threaded, caller-synchronized. The listener is invoked
synchronously, inline, from `apply()`/`apply_batch()`; a listener must not
re-enter `apply()`. `ListenerT` is held by reference — the engine does not
own or extend its lifetime, the caller does.

### Wire encoding vs. stored state

`L1Update` keeps the wire-shaped zero-quantity encoding ("no active
level") because that is what an L1 feed sends. `L2Update` does **not**
use zero-quantity-means-delete — a real captured Deribit `book.*.raw`
session (2026-09-17, see decisions/0001) showed each entry is an
explicit `[operation, price, quantity]` tuple with `operation` one of
New/Change/Delete, and this ADR's first draft had assumed the zero-qty
encoding without checking that against the wire; `L2Update` carries an
explicit `L2Operation` field instead. Regardless of wire encoding, the
query surface and internal storage use `std::optional`, never a sentinel
value — a sentinel leaking into the read API would mean "meaningful-
looking but actually absent" data downstream.

### Generic L3 book vs. Kraken's L3 feed

The generic `L3Policy` knows nothing about any one exchange: it applies
add/modify/delete by `order_id`, keeps orders in arrival order per level,
and reports crossed books and unknown orders. Everything Kraken-specific
about its `level3` feed lives in `KrakenL3Policy` (`kraken_l3_policy.h`),
which composes an `L3Policy` and adds exactly three things:

1. **Checksum verification** (see "L3 checksum" below).
2. **Add ordering** by each order's own timestamp (below).
3. **The subscribed depth**, passed through to the generic book.

`KrakenL3Update` is an `L3Update` plus the timestamp; the generic
`L3Update` has none. The engine takes a policy by value, so a policy that
needs construction-time settings is passed in configured:
`OrderBook<KrakenL3Policy, Listener> book(listener, KrakenL3Policy(depth))`.
`ApplySnapshot`/`ApplyBatch` take a variadic message-meta (zero or one
argument): none for the generic L3 book, a `ChecksumMeta` for Kraken's, a
`ChangeIdMeta` for L2's. A checksum check inside the generic book, or a
listener doing it after the fact, were both considered instead: the
former puts one exchange's protocol in the shared book, and the latter
cannot work because a listener has no batch boundary to hook and cannot
move the book to `Desynced`.

### Depth-limited books

Some feeds deliver a fixed-depth view and never send a delete for a level
that falls out of it. Kraken `level3` is one — confirmed against a real
captured session, plan T7, and stated in Kraken's own docs: "there will be
no `delete` event for price levels that fall out of scope." The client is
responsible for truncating, so `L3Policy` takes a `max_depth` (unlimited
by default) and truncates both sides to it after every batch and after a
fresh snapshot — a snapshot can itself arrive deeper than the subscribed
depth. The depth is a property of the subscription, so it is configuration,
not a constant: whatever builds the subscription passes the same value to
`KrakenL3Policy`, which deliberately has no default. (Kraken's default when
the subscribe message carries no depth is 10, which is what the checked-in
capture was taken with. The *checksum's* window is always the top 10 levels,
`kKrakenChecksumLevels`, independent of the subscribed depth — that one is a
constant of Kraken's algorithm.)

Skipping the truncation isn't just a checksum-scope nicety: an order that
drops out of the tracked window and later re-enters gets a fresh `Add` for
the *same* `order_id` (Kraken's docs: "all orders in the next available
level will generate an add event"), and a book that never truncates keeps
a stale copy around, so the "fresh" add corrupts state by duplicating it.
The generic book also handles the duplicate directly (an `Add` for a known
`order_id` replaces the tracked copy). This was invisible in the golden
book's original hand-written unit tests (which never had a book deep
enough to exercise it) and only surfaced against the real capture, at
message 116 of 6361 — several minutes in.

A second, unrelated real-data finding: multiple orders entering the
tracked window in one message are not guaranteed to be listed in true
arrival order, and applying them in JSON array order gets queue priority
(and the checksum) wrong. `KrakenL3Update` carries each order's own
`timestamp` for exactly this reason — `KrakenL3Policy::ApplyBatch`
reorders only the `Added` events among themselves, by timestamp, and
reinserts them at the positions Add events originally occupied in the
message; non-Add events keep the position they arrived in, both relative
to each other and to the surrounding Adds. That last part matters: a
message can carry an `Added` immediately followed by a `Modified`/`Deleted`
of that same `order_id`, and treating Add/non-Add ordering as fully
independent (hoisting every non-Add before every Add, rather than only
reordering the Adds in place) would apply that later event before the
order it targets exists. See `exchanges/kraken.md` for the full writeup of
both findings, including where in the real capture each one first surfaced.

A delete for a never-seen `order_id` (whether from a genuine feed
anomaly or an edge case truncation doesn't fully cover) is an
`UnknownOrder` integrity issue, not silent corruption or a crash.

### Matching axis

`MatchingPolicy` is concept-constrained — a `requires`-clause naming the
minimal shape a matching policy must expose — rather than an unconstrained
template type parameter. As things stand that constraint
(`MatchingPolicyConcept`) only requires a single `MatchingEnabled() -> bool`
method; it exists so the engine's template parameter has *some* named
concept rather than none, but it does not yet meaningfully constrain what a
real matching policy would need to expose (an order-submission entry point,
fill notifications — see below). It will need to grow substantially once an
actual matching implementation is designed. Only `NoMatchingPolicy` is
implemented now: it applies a diff exactly as given, on the premise that
the feed's diffs are already post-trade truth ("trust the feed"). The
actual matching algorithm (price-time priority, self-match handling, etc.)
is a distinct, larger subsystem shared with the future "exchange book
simulating other participants" use case, and is explicitly out of scope
here.

One thing is named now without being built: a matching-capable book will
need a second entry point (`submit_order`) and trade/fill notification
hooks that the current listener concepts don't model. Naming this now
means the listener concepts aren't frozen as though only feed events will
ever exist, without committing to a matching algorithm design prematurely.

### L3 checksum: algorithm and placement

The Kraken `level3` checksum is computed by a free function
(`kraken_checksum.h`/`.cpp`) over the golden book's `BookView` and verified
by `KrakenL3Policy`, so neither the generic engine nor the generic
`L3Policy` references it — they only know the feed-agnostic
`on_integrity_check_failed(ChecksumMismatch)` hook, never a Kraken-specific
one. This matches the project's own layering: exchange-specific behavior
belongs in the exchange-specific piece, not the shared engine.

decisions/0001 confirms the `checksum` field exists on Kraken's wire but
not the formula computing it, and it is a *different* formula from the
`book`/L2-channel checksum — the L3 book must keep individual orders per
price level in a deterministic, arrival-ordered structure (not
aggregate-only counts), since Kraken's `level3` checksum is defined over
individual top orders per side. Primary validation is a real ~2-minute
captured Kraken session (via the real `kraken_feed_handler` binary, not a
Python probe) replayed end-to-end, zero checksum mismatches across the 1
snapshot and 6361 update messages that carry a checksum (6362 of the
capture's 6496 total wire records; the rest is non-`level3` control
traffic — status, subscribe ack, heartbeat) once the depth-truncation and
arrival-order findings above were folded in
(`src/order_book/tests/kraken_capture_replay_test.cpp`); Kraken's own
documented worked example is the independent known-answer check
(`src/order_book/tests/kraken_documented_example.h`, used by
`kraken_checksum_test.cpp` and `kraken_l3_policy_test.cpp`). The snapshot's
own checksum is verified the same way the update messages are —
`KrakenL3Policy::ApplySnapshot` takes a `ChecksumMeta` and returns a
`ChangeSet` like `ApplyBatch` does, rather than applying the snapshot
unconditionally and leaving the caller to check it separately.

### Price and quantity scale

`Price` and `Quantity` are integer ticks and lots; an exchange sends
decimals. `InstrumentScale` (`instrument_scale.h`) does the conversion for a
given number of price and quantity decimals — per-instrument reference data
(Kraken's `pair_decimals`/`lot_decimals`, Deribit's tick size), so it is
supplied by the caller and nothing in this subsystem hardcodes one. The
replay tests and `kraken_journal_dump` state which instrument's settings
they use; the dump tool takes them as flags. `kraken_l3_wire.h` is the one
place Kraken's `level3` JSON is turned into `KrakenL3Update`s, shared by the
tests and the tool.

### Test fixtures

The two replay fixtures are real captured sessions. They are committed
compressed (`src/order_book/tests/data/capture_fixtures.tar.xz`, ~290 KB
against ~3.9 MB raw) and unpacked into the build tree at CMake configure
time, so the tests read plain files and the repo does not carry the raw
dumps. Git LFS was considered and not needed at this size.

### Deribit `change_id`/`prev_change_id`: resolved by capture

Previously an open assumption (only Deribit's FIX probe, which has no
`change_id`, had been run against this repo). Settled 2026-09-17 by a
real ~2-minute capture of `book.BTC-PERPETUAL.raw` (see decisions/0001):
`prev_change_id` matched the previous message's `change_id` with zero
gaps across 2069 consecutive messages, replayed end-to-end in
`src/order_book/tests/deribit_capture_replay_test.cpp` with zero `Gap`,
`CrossedBook`, or `UnknownLevel` integrity issues reported. Gap detection
stays a listener notification plus a state transition, not a hard
exception baked into control flow, since a longer or more adverse capture
could still reveal a real gap case the current design needs to handle
differently.

### Build integration: follows the `feed_handler` precedent

Written against a stale local `main` that predated `decisions/0004`
(feed handler) and `decisions/0005` (quality gates) landing — this ADR's
first draft proposed a new `add_project_library()` CMake helper and a
top-level `include/`+`tests/` layout. Once rebased onto the real `main`,
neither matched what had actually been established in the meantime:
library targets are plain `add_library()` + manual
`project_options`/`project_warnings`/`project_sanitizers` linkage (see
`feed_handler` in `CMakeLists.txt`), test binaries go through
`add_project_test()`, and everything lives under `src/<component>/`
(headers and sources together, tests in `src/<component>/tests/`) rather
than a top-level `include/`. `order_book` now follows that precedent
exactly rather than inventing a second convention.

## Consequences

- The golden book's correctness depends on getting the `ChangeSet`/engine
  contract right early (built out alongside L1, the least representative
  granularity — no map, no order IDs, no checksum, no depth truncation);
  if L2/L3 reveal it doesn't generalize, the engine gets adjusted rather
  than forcing L3 into an ill-fitting shape.
- The matching seam is a placeholder, not a finished design — the real
  risk of getting matching wrong is deferred to a future plan, not
  eliminated by naming `submit_order`/fill hooks here.
- The Deribit `change_id` assumption is resolved (see above) — a real
  capture also caught a real bug in this ADR's first draft (`L2Update`'s
  wire encoding), underscoring that a captured-session replay test is
  worth more than a docs-derived assumption even when the docs turn out
  to be right about the part they were asked about.
- Fast, exchange-specific implementations, the actual matching engine, and
  profiling/benchmarking are all explicitly out of scope for the golden
  reference and are follow-on work once it lands and is validated.
