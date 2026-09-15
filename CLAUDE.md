# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A from-scratch, latency-focused tick-to-strategy pipeline for crypto markets
(exchange feed capture → order book reconstruction → strategy hook), built as
a portfolio piece to demonstrate HFT-style systems engineering — concurrent
feed ingestion, deterministic replay/journaling, low-latency data structures,
profiling discipline — against real exchange data rather than a simulation.

**Status: early stage.** Only a CMake skeleton (`src/greeter.*`, `src/main.cpp`)
and throwaway Python protocol probes (`experiments/`) exist so far. No feed
handler, order book, or strategy code has landed yet. `decisions/` holds the
locked-in ADRs; the rest of the design is intentionally unspecified and will
be worked out in future sessions — don't assume unwritten components exist.

**Always read `decisions/*.md` before making an architectural call.** They
are the actual design doc for this project (in place of scattered markdown
files elsewhere) and record *why* choices were made, not just what they are.
Add a new ADR there for any future non-obvious architectural decision instead
of leaving the rationale only in a commit message or chat.

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

### Lint / format

Enforced via pre-commit (`pre-commit install` once per clone):

```bash
pre-commit run --all-files
```

- `clang-format` (Google-based, 100-col, 4-space indent, left-aligned
  pointers — see `.clang-format`) runs standalone.
- `clang-tidy -p build --quiet` and `cppcheck` both need
  `build/compile_commands.json` to exist first (configure any preset once).
- `.clang-tidy` enables `bugprone-*`, `performance-*`, `modernize-*`,
  `readability-*`, `cppcoreguidelines-*`, `clang-analyzer-*` (with a short
  explicit exclude list already tuned — e.g. magic-numbers and trailing
  return type checks are off). **Prefer tightening `.clang-format`/
  `.clang-tidy` over adding prose style docs** when a stylistic preference
  comes up — that's the intended mechanism for keeping LLM-authored code
  consistent here, not more markdown.

### Tests

Not wired up yet — GoogleTest is the chosen framework (`decisions/0002`) but
no `CMakeLists.txt` test target or `FetchContent` declaration exists yet.
TDD is the intended workflow for feed-parsing/order-book logic once real
code starts landing; set up the GTest target as part of that first slice of
work rather than assuming it's already there.

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
