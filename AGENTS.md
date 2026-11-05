# AGENTS.md

This file provides guidance to coding agents (Claude Code, Codex, etc.) when working with code in this repository. `CLAUDE.md` is a symlink to it: edit this file.

## What this is

A from-scratch, latency-focused tick-to-strategy pipeline for crypto markets
(exchange feed capture → order book reconstruction → strategy hook), built as
a portfolio piece to demonstrate HFT-style systems engineering — concurrent
feed ingestion, deterministic replay/journaling, low-latency data structures,
profiling discipline — against real exchange data rather than a simulation.

**Status: early stage.** What exists: throwaway Python protocol probes
(`experiments/`), the first vertical slice of the feed handler in
`src/feed_handler/` — the `MessageSink` seam, the v1 capture journal (writer + reader), the Kraken REST
auth / `AssetPairs` client, the Kraken `level3` WebSocket client, and the
`feed_handler` binary that captures live market data to a journal. It takes
`--config <path>`, a TOML file listing `[[connections]]`, one per socket and
journal, each with its own symbols (`config/feed_handler.toml`,
`docs/modules/feed-handler.md`), and runs every entry across both exchanges,
optionally narrowed with `--exchange` and `--only`; credentials come from the
environment and it runs until SIGINT. The second exchange backend is
complete to the same depth: `src/feed_handler/fix/` (generic hand-rolled
FIX.4.4 builder/parser/framer) and `src/feed_handler/deribit/` (session
mechanics plus a raw-POSIX-socket client on its own thread), which logs on to
Deribit's FIX testnet, subscribes to the configured instruments and journals
into the configured `journal_dir`. Each connection journals on its own
thread behind an SPSC ring, so no connection thread touches the disk
(`decisions/0010`). A golden
(deliberately simple, `std::map`-based) order book also exists in
`src/order_book/` (L1/L2/L3 granularity, no matching yet, see
`decisions/0006`), and `src/book_adapter/` wires it to both exchanges' feeds:
live in the capture binary behind `order_books = true` (default off), on one
book thread fed by a ring per connection, and offline through the
`journal_replay` tool, which replays a captured journal through the same adapter
for profiling (`decisions/0008`, `docs/modules/order-book.md`). The fast
exchange-specific books, the matching engine, and strategy code all remain
unwritten. Don't assume unwritten components exist.

**Where the design lives:**

- `decisions/*.md`: ADRs, only for decisions that are expensive to reverse,
  each with its options and trade-off. All are `proposed` (or `provisional`)
  unless marked `accepted`: working assumptions to follow, and to challenge
  with a proposal when work shows one wrong. Read them before an architectural
  call.
- `docs/architecture.md`: how the pieces fit, dependencies, and choices that
  are cheap to change. Meant to change as the project learns.
- `docs/modules/` (what each component is and how it behaves), `docs/tasks/`
  (how to do things), `docs/investigations/` (dated evidence).
- `docs/questions/`: one file per open question nobody has answered yet. A
  session that hits one it cannot settle writes a file there, records what it
  assumed meanwhile, and carries on.

A new decision gets an ADR only if reversing it would be expensive (a contract
between components, a stored format, a threading or ownership model, a pattern
that spreads); anything else goes in `docs/architecture.md` or a module doc.
Never leave the rationale only in a commit message or chat.

**`exchanges/*.md` holds raw per-exchange protocol reference** (endpoints,
auth flow, message shapes, confirmed wire quirks) — facts, not rationale.
When an ADR references an exchange behavior, the *why* stays in the ADR and
the *what* (exact fields, endpoints) belongs in `exchanges/`; update it
whenever a probe in `experiments/` confirms something new about the wire
format. See `exchanges/README.md` for the index.

- `decisions/0001-exchange-feeds.md` — why the project builds against
  Kraken `level3` (L3, per-message checksum, no sequence numbers), Bitso
  `diff-orders` (a second L3 with +1 sequence numbers and REST-snapshot
  recovery, not built yet, #18) and Deribit FIX market data (L2 with FIX
  session sequencing, a second protocol surface). None ranks above the others.
  Deribit SBE multicast is a stretch goal; wire facts live in `exchanges/`.
- `decisions/0002-no-cxx-modules.md` — plain `.h`/`.cpp`, not C++20 modules
  (provisional): Clang 21 + CMake + Ninja build modules fine, but clangd's
  cross-file navigation stops at an `import`, and reliable navigation is a hard
  requirement. Don't introduce modules until its revisit trigger fires. The
  dependency list and why each is there is `docs/architecture.md`.
- `decisions/0004-raw-frame-sink-seam.md` — exchange-facing code hands raw
  wire frames to `MessageSink`s as non-owning views; a sink copies what it keeps,
  and every stateful sink resets at a new `connect_id`.
  This seam is what keeps strategy-facing code identical across
  LiveTrading/Replay/Simulation (`docs/architecture.md`).
- `decisions/0006-order-book-policies.md` — books vary by granularity
  (L1/L2/L3) and matching through concept-constrained policy templates, not
  virtual dispatch or CRTP; exchange quirks (Kraken's checksum, depth
  truncation, add ordering) live in exchange policies composing generic ones.
  How the books behave in detail is `docs/modules/order-book.md`.
- `decisions/0008-book-adapter-layering.md` — `order_book` and
  `feed_handler` never depend on each other; `src/book_adapter/` depends on
  both and is linked only by binaries that want books. `feed_handler` offers
  generic hooks (`AddSink`, `BeforeStart`) instead of knowing about books.
- `decisions/0009-journal-content-and-format.md` — the journal records inbound
  wire bytes verbatim, one file per connection and `connect_id`, versioned and
  crash-tolerant, never outbound traffic (credentials). Explicitly not settled:
  revisit once replay has to reproduce the live order of book updates across
  connections, or when Bitso's REST snapshot has to be journaled
  (`docs/questions/`).
- `decisions/0010-connection-thread-handoff.md` — connection threads never
  block or touch disk, and do not parse until feed books move onto them; one
  SPSC ring per consumer, each owning its full-ring policy (set in 0011 and
  0012); staged towards epoll thread groups that run the feed books inline,
  once fast feed books exist and IXWebSocket is replaced.
- `decisions/0011-desync-recovery.md` — any lost data (gap, checksum,
  silence, dropped frame, integrity issue) desyncs, and recovery is always a
  reconnect plus a fresh snapshot of that connection only (a full book ring
  drops and desyncs). What `connect_id` means is in 0004.
- `decisions/0012-failure-containment.md` — a failure stays at its own level
  (book < connection < process); a journal failure stops that connection's
  journal and alerts, it does not stop other connections; only disk/memory
  exhaustion escalates, into a maintenance mode not designed yet. The code
  still stops every connection on any fatal.
- `decisions/0013-golden-reference-book.md` — a deliberately simple
  `std::map`-based golden book is the oracle: fast books are checked against it
  on the same inputs, and new features land in it first.
- `decisions/0014-normalization-boundary.md` — a validating feed book per
  connection is where exchange data becomes normalized: exchange quirks and
  integrity checks live in its exchange policies, and everything downstream
  (strategy books, strategies, the simulator's consumers) sees only normalized
  events.

## Commands

### Build

CMake presets drive everything; each preset gets its own `build/<preset>` dir:

```bash
cmake --preset debug      # Debug, no optimizations
cmake --preset release    # -O3, ICF
cmake --preset profile    # release-equivalent + gperftools CPU profiler linked in
cmake --preset production # release-equivalent + ThinLTO
cmake --preset coverage   # Debug + --coverage instrumentation, -O0

cmake --build build/debug
```

`compile_commands.json` is exported by every preset (needed by clangd and by
the pre-commit clang-tidy/cppcheck hooks) — configure at least once before
either will work.

Only Clang is exercised (`CMAKE_CXX_COMPILER: clang++` is hardcoded in every
preset; `lld` is the linker). Toolchain versions are pinned to `-21` — see
`scripts/bootstrap.sh` for the exact package list and why (matches
`update-alternatives` targets for `clang-format`/`clang-tidy`).

Useful CMake options (pass as `-D<OPTION>=ON` or add to a preset's
`cacheVariables`): `ENABLE_ASAN`, `ENABLE_UBSAN`, `ENABLE_TCMALLOC`,
`ENABLE_GPERFTOOLS` (already on in the `profile` preset), `ENABLE_COVERAGE`,
`ENABLE_FUZZER` (libFuzzer targets, `docs/tasks/fuzzing.md`), `USE_CLOCK_MANAGER`.

### Lint / format / quality gates

**Rule for agents:** code must pass every automated check before it is
committed or pushed. Never bypass a hook (`--no-verify`) and never weaken a
check, a lint rule or a test to make something pass. Adding a check needs no
permission; removing or loosening one needs the owner. Silencing a warning is
allowed only at the line that triggers it (e.g. `// NOLINT(<check>)`), with a
comment saying why, never file-wide or project-wide.

Two `pre-commit` framework hook stages; `docs/tasks/testing.md` has what each
runs, why, and what is deliberately not enforced yet:

```bash
pre-commit install                      # commit-time hooks
pre-commit install --hook-type pre-push # push-time hooks (sanitizers)
pre-commit run --all-files              # commit-time hooks, on demand
```

Commit-time (`.pre-commit-config.yaml`, default stage):
- `clang-format` (Google-based, 100-col, 4-space indent, left-aligned
  pointers — see `.clang-format`) runs standalone.
- `clang-tidy -p build/debug --quiet` and `cppcheck` both need
  `build/debug/compile_commands.json` to exist first (`cmake --preset debug`
  once).
- `.clang-tidy` enables `bugprone-*`, `performance-*`, `modernize-*`,
  `readability-*`, `cppcoreguidelines-*`, `clang-analyzer-*` (with a short
  explicit exclude list already tuned) plus `readability-identifier-naming`
  (Google-style: `CamelCase` types/functions/methods, `lower_case`
  variables/params/namespaces, `k`-prefixed constants, trailing-underscore
  private members). **Prefer tightening `.clang-format`/`.clang-tidy` over
  adding prose style docs** when a stylistic preference comes up — that's
  the intended mechanism for keeping LLM-authored code consistent here, not
  more markdown.
- `scripts/check_build.sh` — an actual incremental `cmake --build build/debug`.
  A commit must at least compile.

Push-time (`stages: [pre-push]`):
- `scripts/run_sanitizers.sh` — the full test suite under ASan+UBSan, then
  again under ThreadSanitizer, each in its own build directory. Slow by
  design; catches exactly the bug classes (use-after-free, data races) that
  went unnoticed in this codebase until sanitizers were actually run.

### Tests

GoogleTest (`docs/architecture.md`), fetched via `FetchContent` and registered with
ctest:

```bash
ctest --test-dir build/debug
./build/debug/bin/feed_handler_tests   # or run a binary directly
```

The order-book replay tests read real captured sessions that are committed
compressed and unpacked into the build tree at configure time; see
`docs/modules/order-book.md`.

Add new test binaries with `add_project_test(name sources...)` (the test
equivalent of `add_project_executable`). TDD is the intended workflow for
feed-parsing/order-book logic.

The pre-commit cppcheck hook runs with `--library=googletest`, which teaches
it `TEST`/`TEST_F`'s macro expansion — helpers, constants, and tests can live
in any order inside an anonymous namespace without tripping a spurious
`syntaxError`.

## Architecture notes

- Every executable target must go through the `add_project_executable()`
  CMake function (`CMakeLists.txt`; the libFuzzer target is the one exception,
  `docs/tasks/fuzzing.md`) rather than raw `add_executable()` — it
  links `project_options`/`project_warnings` (and `gperftools_profiler` when
  `ENABLE_GPERFTOOLS` is on) so no target can accidentally skip warnings or
  the shared compiler/link flags. Add new targets this way.
- `project_warnings` (`-Wall -Wextra -Werror -Wshadow -Wconversion
  -Wsign-conversion` + a short suppress list) applies uniformly; don't
  special-case a file's warnings unless there's a genuinely unfixable
  third-party-header cause. `FetchContent_Declare` deps are marked `SYSTEM`
  so their headers never trip it.
- `project_options` carries the "how does this binary get built" concerns
  (LTO in `production` only, ICF in `release`/`profile`/`production`, the
  `-ftime-trace` per-TU trace for ClangBuildAnalyzer) — extend it, not
  per-target flags, for anything that should apply build-wide.
- `experiments/` holds throwaway Python protocol probes
  (`kraken_l3_probe.py`, `deribit_fix_probe.py`) used to verify real wire
  behavior against both exchanges before building the real C++ pipeline —
  they are explicitly **not** the real design, just probes; findings from
  running them are captured into `exchanges/` (the ADR keeps only the
  reasoning). They read exchange credentials from
  `experiments/.env` (gitignored) via a minimal hand-rolled dotenv loader —
  **never open or read `experiments/.env`** (only `.env.example` is safe to
  view). Captures from authenticated sessions can contain the auth handshake
  too (the probes no longer write it): never print a capture's contents to
  check it; look at its keys or count matches instead. A private venv lives at
  `experiments/.venv`.
- **Captured market data stays out of git.** Journals and JSON dumps from a
  capture are gitignored (`journal/`, `*.journal`). A test fixture is the one
  exception: it is committed compressed (`.tar.xz`) and small, then unpacked at
  configure time (see `docs/modules/order-book.md`). A large capture, such as an
  hour recorded for profiling, lives outside the repo in `~/Temp`, not `/tmp`.
