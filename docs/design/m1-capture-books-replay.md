# M1: Capture, golden book, replay

- Roadmap entry: `docs/roadmap.md`, M1.
- Design status: ready (simplify-design reviewed 2026-11-04).

## Goal and exit test

Record live market data from Kraken and Deribit, rebuild the order books from
it live, and replay a recorded session through the same books offline, getting
the same books.

**Exit test.** Run `feed_handler` for one hour on `config/feed_handler.toml`
with `order_books = true` and a book dump directory. Then replay each
connection's journals twice with `journal_replay`, each run dumping its books.
It passes when all of these hold:

1. Every connection's summary line reports zero dropped frames, zero integrity
   issues (checksum, gap, crossed book, unknown order or level), zero parse or
   apply errors, and `BookService::InternalErrors()` is zero.
2. The two replay dumps are byte-identical.
3. The replay dump is byte-identical to the live dump, for every connection.

One script runs steps 2 and 3 and exits non-zero on any difference (see
Interfaces). Condition 1 is read from the live run's log.

## What already exists

Almost all of M1 was built by #14 (merged in #16): capture with one thread per
connection and a journal thread per connection behind SPSC rings
(`decisions/0010`), the golden books (`decisions/0013`), the book adapter
(`decisions/0008`), live books behind `order_books = true`, and `journal_replay`
with counts and timing. What is missing is the comparison: neither the live run
nor the replay can write out its final books, so "replay matches live" cannot
be checked.

## Scope

- A deterministic text dump of every book the adapter holds.
- `--dump-books <dir>` on `feed_handler` and on `journal_replay`.
- A script that runs the replay twice and diffs the dumps against each other and
  against the live dump.
- Running the exit test once and recording the result.
- Closing #14 once its checklist is confirmed against the merged code.

## Non-goals

- Journal failure containment (#17): the exit test passes without it, so by the
  roadmap's scope gate it stays in the backlog.
- Comparing books at points during the run, not only at the end (see Open
  questions).
- Journal rotation and format v2 (#5), the k-way merge (#6): the comparison is
  per connection, and each connection's books depend only on that connection's
  frames, so replay order across connections does not matter here. It does
  from M3, where a strategy reads several connections
  (`docs/questions/2026-09-23-replay-order-and-journal-content.md`).
- Bitso (M2), normalized events (M2, `decisions/0014`).
- Changes to how books are built or validated, unless the exit test fails
  because of a book bug. That fix lands in M1 under the roadmap's scope gate.

## Components touched

- `src/book_adapter/`: a new `book_dump.h`/`.cpp` that writes every book of one
  connection from a `BookAdapter`. Lives here because it reads the adapter and
  both exchanges' book types (`decisions/0008`).
- `src/feed_handler/feed_handler_main.cpp`: the `--dump-books` flag; the dump is
  written after `books.Stop()`, next to `LogSummaries()`, when the flag is given
  and `order_books` is on. The flag with `order_books` off is a usage error, since no dump
  would then look the same as an empty, passing one.
- `src/book_adapter/tools/journal_replay.cpp`: the `--dump-books` flag; the dump
  is written after the last journal.
- `scripts/check_replay_matches_live.sh`: the comparison script.
- Docs: `docs/modules/order-book.md` (the dump format),
  `docs/tasks/running-a-capture.md` (how to run the exit test), and a dated
  `docs/investigations/` note with the exit-test result.

## Interfaces and data

### Dump layout

```
<dir>/<connection id>/<symbol>.book
```

`<connection id>` is the `id` of the `[[connections]]` entry, the same `<id>`
that starts the journal file names. One file per book the adapter holds when
the dump is written. A symbol that never received a snapshot has no book and no
file, in both runs alike.

`journal_replay` takes the connection id from its journals' file names; journals
of more than one id in one invocation are a usage error, because `journal_replay`
applies every frame to one adapter connection, so two ids would merge into one
set of books.

### Dump file format

Plain text, one item per line, `\n` endings, integers only (prices and
quantities in the book's ticks and lots, never formatted decimals), so two
dumps compare with `diff` and a difference is readable.

```
book <connection id> <symbol>
connect_id <n>
readiness <Uninitialized|Ready|Desynced>
issues <name>=<count> ...            # every IntegrityIssue, fixed order, zeros included
bids <level count>
L <price> <quantity>                 # best first
O <order id> <quantity>              # L3 only: the level's orders in queue order
asks <level count>
L <price> <quantity>
O <order id> <quantity>
```

L2 books (Deribit) have no `O` lines. L3 books (Kraken) list every order under
its level, so a difference in queue order shows up even when level totals agree.
Order ids are the book's `OrderId` values, read through `L3Policy::Levels()` and
`OrderQuantity()`.

Event counts (messages, updates, timing) are not in the dump: they differ
legitimately between a live run and a replay.

### Functions

```cpp
namespace book_adapter {

// Writes one file per book of `handle` under `dir/<connection id>/`, creating
// the directories. Returns an error message on any I/O failure.
std::expected<void, std::string> DumpBooks(const BookAdapter& adapter,
                                           ConnectionHandle handle,
                                           const std::filesystem::path& dir);

// New on BookAdapter: the symbols that have a book on this connection, sorted,
// so the dump can enumerate them in a fixed order. The books stay private
// unordered_maps; this is the only new read access.
[[nodiscard]] std::vector<std::string_view> BookAdapter::Symbols(ConnectionHandle handle) const;

// The text of one book's file, for tests.
std::string FormatBookDump(std::string_view connection_id, const KrakenBook& book);
std::string FormatBookDump(std::string_view connection_id, const DeribitBook& book);

}  // namespace book_adapter
```

### Script

```
scripts/check_replay_matches_live.sh <live dump dir> <journal dir>
```

For each connection directory in the live dump: run `journal_replay
--dump-books` twice on that connection's journals (in `connect_id` order, with
the connection's decimals and depth), `diff -r` the two replay dumps, then
`diff -r` replay against live. Prints one line per connection, exits 1 on any
difference.

## Flows

- **Main path.** Live: connections, rings, book thread, `Stop()` drains the rings,
  then the dump. Replay: the journals in order, inline, then the dump. Both apply
  the same frames to the same adapter code, so the dumps match.
- **Reconnect during the hour.** A connect record erases the connection's books
  in both runs. The dump shows the last `connect_id`'s books in both. Covered by
  a test with two `connect_id` journals.
- **Dropped frames in the live run** (full book ring). The live book desyncs;
  the journal has every frame, so the replayed book does not. The dumps differ in
  `readiness` and probably in content. Exit-test condition 1 rules this out; the
  script's diff makes it visible if it happens.
- **A checksum mismatch or gap in the live run.** Both runs see the same frames,
  so both books desync at the same point and the dumps still match. Condition 1
  still fails the exit test.
- **Process killed instead of stopped.** No live dump is written; the exit test
  cannot run. The journal's torn tail is the reader's concern (`decisions/0009`).
- **Dump directory not writable.** `feed_handler` logs the error and exits
  non-zero after the capture has ended; the journal is unaffected.

## Decisions

- `decisions/0008`: the dump lives in `book_adapter`.
- `decisions/0010`: the book thread owns the books, so the dump runs after
  `BookService::Stop()` joins it.
- `decisions/0013`: the dump reads the golden book's public queries (`BookView`,
  plus `L3Policy::Levels()`/`OrderQuantity()` for queue order), never a
  policy's internals.

No new ADR: the dump format is a debugging and test artifact, not stored data
anything depends on, and can change in one commit.

## Assumptions about later milestones

- Fast books (after v0) are checked against the golden book on the same inputs
  (`decisions/0013`). This dump is a natural way to do that; if they need a
  finer comparison (per message), that is new work, not a change to this one.
- M2 adds Bitso, whose L3 book dumps in the same format with `O` lines. Cost if
  Bitso's book exposes a different order identity: a third `FormatBookDump`
  overload.
- Normalized events (`decisions/0014`) do not change what is dumped: the feed
  book stays the thing being compared.

## Open questions

- **Final-state comparison only** (non-blocking). Two runs could diverge midway
  and converge again, for example both erased by a reconnect. Final state
  catches every divergence that survives to the end, which is what the exit test
  asks. A per-message digest would localize a divergence; add it only if a
  mismatch turns out hard to find.
- **What the summary line reports** (non-blocking). If it lacks one of the
  counters condition 1 needs, the implementer adds it.
- **How the script finds a connection's decimals and depth** (non-blocking).
  From the config file or from flags; the implementer picks one and documents
  it in `docs/tasks/running-a-capture.md`.
