# AGENTS.md

This file provides guidance to coding agents (Claude Code, Codex, etc.) when working with code in this repository. `CLAUDE.md` is a symlink to it: edit this file.

## What this is

A from-scratch, latency-focused tick-to-strategy pipeline for crypto markets
(exchange feed capture → order book reconstruction → strategy hook), built as
a portfolio piece to demonstrate HFT-style systems engineering — concurrent
feed ingestion, deterministic replay/journaling, low-latency data structures,
profiling discipline — against real exchange data rather than a simulation.

**Status: early stage.** What exists: the CMake skeleton (`src/greeter.*`,
`src/main.cpp`), throwaway Python protocol probes (`experiments/`), and the
first vertical slice of the feed handler in `src/feed_handler/` — the
`MessageSink` seam, the v1 capture journal (writer + reader), the Kraken REST
auth / `AssetPairs` client, the Kraken `level3` WebSocket client, and the
`kraken_feed_handler` binary that captures live market data to a journal
(`KRAKEN_API_KEY`/`KRAKEN_API_SECRET` from the environment, journals into
`./journal/`, runs until SIGINT). The second exchange backend is
complete to the same depth: `src/feed_handler/fix/` (generic hand-rolled
FIX.4.4 builder/parser/framer), `src/feed_handler/deribit/` (session
mechanics plus a raw-POSIX-socket client on its own thread) and the
`deribit_feed_handler` binary, which logs on to Deribit's FIX testnet
(`DERIBIT_TESTNET_CLIENT_ID`/`DERIBIT_TESTNET_CLIENT_SECRET` from the
environment), subscribes to `BTC-PERPETUAL` and journals into the same
`./journal/` until SIGINT. No order book or strategy code has landed yet. `decisions/` holds the locked-in ADRs; the rest of the design is
intentionally unspecified and will be worked out in future sessions — don't
assume unwritten components exist.

**Always read `decisions/*.md` before making an architectural call.** They
are the actual design doc for this project (in place of scattered markdown
files elsewhere) and record *why* choices were made, not just what they are.
Add a new ADR there for any future non-obvious architectural decision instead
of leaving the rationale only in a commit message or chat.

**`exchanges/*.md` holds raw per-exchange protocol reference** (endpoints,
auth flow, message shapes, confirmed wire quirks) — facts, not rationale.
When an ADR references an exchange behavior, the *why* stays in the ADR and
the *what* (exact fields, endpoints) belongs in `exchanges/`; update it
whenever a probe in `experiments/` confirms something new about the wire
format. See `exchanges/README.md` for the index.

- `decisions/0001-feed-source-selection.md` — why Kraken's public L3 `level3`
  WS feed (genuine order-by-order `add`/`modify`/`delete` by `order_id`) is
  the primary feed, with Deribit's L2 `book` channel (real `change_id`/
  `prev_change_id` sequencing) as the secondary/differently-shaped feed
  problem. Also documents empirically-confirmed wire details worth knowing:
  Kraken `level3` has no sequence numbers but *does* carry a per-message
  `checksum` for desync detection; Deribit's WS `book` and FIX market data
  are both L2, not L3, despite FIX being a genuinely different protocol
  surface. A real Deribit SBE/UDP-multicast colo feed exists but is gated
  behind infra/cost not worth taking on yet (stretch goal).
- `decisions/0002-tooling-stack.md` — concurrency primitives
  (moodycamel `concurrentqueue`/`readerwriterqueue`), containers (Abseil /
  ankerl hash & btree maps), testing (GoogleTest, not Boost.Test), profiling
  (`perf c2c`, Valgrind, gperftools/tcmalloc — Linux-only), serialization
  (deliberately deferred until a concrete wire/journal format is needed).
  Also: C++20 modules were prototyped and confirmed working in Clang 21 +
  CMake + Ninja, but **de-scoped back to plain `.h`/`.cpp`** because clangd's
  modules support is experimental and cross-TU Find-References/Go-to-Definition
  doesn't work across an `import` boundary — reliable navigation is a hard
  project requirement. Don't reintroduce modules until clangd support matures.
- `decisions/0003-dev-environment.md` — WSL2 (Ubuntu, native ext4, never
  `/mnt/c/...`) for day-to-day dev now; native dual-boot Linux planned later
  specifically because `perf c2c`/PMU access is unreliable under WSL2's
  Hyper-V. Until dual-boot exists, any profiling that specifically needs
  `perf c2c` should go to a disposable cloud VPS, not WSL2.
- `decisions/0004-feed-handler-architecture.md` — the three execution modes
  (LiveTrading/Replay/Simulation) and the `MessageSink` interface (with its
  frame-ownership contract) that keeps strategy-facing code identical across
  them; the v1 journal format (raw wire bytes + capture metadata, framed and
  versioned, one file per exchange+connection-incarnation — not per symbol);
  the threading model (epoll-per-thread-group is the end-goal, but
  IXWebSocket owning its own fd/thread keeps Kraken a standalone exception
  until it's replaced) and where the future SPSC fan-in seam goes; and the
  per-exchange snapshot/recovery/gap-handling recap (detailed wire facts
  live in `exchanges/`, not here).
- `decisions/0005-quality-gates-and-release-process.md` — the commit-time
  (must compile) and push-time (full suite under ASan+UBSan and TSan)
  enforcement tiers, why MemorySanitizer is deferred rather than added, why a
  local git hook isn't a real (unbypassable) gate, and the still-open
  questions around a release process (branching strategy, coverage
  thresholds, perf/simulation checks) — don't assume any of those were
  decided, read the ADR's open-questions section first.

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
`USE_CLOCK_MANAGER`.

### Lint / format / quality gates

Two `pre-commit` framework hook stages — see `decisions/0005` for the full
reasoning behind the split and what's deliberately not enforced yet:

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

GoogleTest (`decisions/0002`), fetched via `FetchContent` and registered with
ctest:

```bash
ctest --test-dir build/debug
./build/debug/bin/feed_handler_tests   # or run a binary directly
```

Add new test binaries with `add_project_test(name sources...)` (the test
equivalent of `add_project_executable`). TDD is the intended workflow for
feed-parsing/order-book logic.

The pre-commit cppcheck hook runs with `--library=googletest`, which teaches
it `TEST`/`TEST_F`'s macro expansion — helpers, constants, and tests can live
in any order inside an anonymous namespace without tripping a spurious
`syntaxError`.

## Architecture notes

- Every executable target must go through the `add_project_executable()`
  CMake function (`CMakeLists.txt`) rather than raw `add_executable()` — it
  links `project_options`/`project_warnings` (and `gperftools_profiler` when
  `ENABLE_GPERFTOOLS` is on) so no target can accidentally skip warnings or
  the shared compiler/link flags. Add new targets this way.
- `project_warnings` (`-Wall -Wextra -Werror -Wshadow` + a short suppress
  list) applies uniformly; don't special-case a file's warnings unless
  there's a genuinely unfixable third-party-header cause.
- `project_options` carries the "how does this binary get built" concerns
  (LTO in `production` only, ICF in `release`/`profile`/`production`, the
  `-ftime-trace` per-TU trace for ClangBuildAnalyzer) — extend it, not
  per-target flags, for anything that should apply build-wide.
- `experiments/` holds throwaway Python protocol probes
  (`kraken_l3_probe.py`, `deribit_fix_probe.py`) used to verify real wire
  behavior against both exchanges before building the real C++ pipeline —
  they are explicitly **not** the real design, just probes; findings from
  running them are captured back into the relevant ADR (see the "Empirical
  validation" section of `0001`). They read exchange credentials from
  `experiments/.env` (gitignored) via a minimal hand-rolled dotenv loader —
  **never open or read `experiments/.env`** (only `.env.example` is safe to
  view). A private venv lives at `experiments/.venv`.
