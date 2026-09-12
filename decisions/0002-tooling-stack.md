# 0002. Core tooling stack

## Context

Coming from a C++/HFT day-job toolchain (boost.test, boost.serialization,
tcmalloc/gperftools, perf/valgrind on Ubuntu, CLion+CMake), picking
equivalents that work well for a solo portfolio project without a company's
infrastructure behind them.

## Decision

- **Concurrency primitives**: [moodycamel](https://github.com/cameron314)
  `concurrentqueue`/`readerwriterqueue` for SPSC/MPMC queues between capture
  threads and the order book — not hand-rolled, matches prior experience.
- **Containers**: Abseil / ankerl hash maps and btree maps rather than
  hand-rolling; not reinventing well-solved data structures.
- **Testing**: Google Test. Equivalent to boost.test (used at the day job);
  switching since boost.test's ecosystem overlaps painfully with
  boost.serialization's known problems (see below), and GTest is the more
  common default outside boost-shops.
- **Build**: CMake + Ninja + Clang, tracking the latest available toolchain
  versions rather than distro defaults.
- **Serialization**: deferred. Boost.serialization was a bad experience at
  the day job (versioning pain); an azpp-style approach worked well there
  and is the leading candidate once an actual wire/journal format is needed
  — not needed yet at the exchange-message-parsing stage.
- **Profiling**: perf (`perf c2c` for cache-line contention), Valgrind,
  gperftools/tcmalloc — all Linux-only tools, which directly shaped the
  development-environment decision (see 0003).
- **Editor**: VS Code + Remote-WSL + clangd. CLion was the prior default but
  the subscription lapses when the current employer closes; VS Code's
  Remote-WSL model was chosen specifically because it keeps the UI native on
  Windows while all indexing/build/terminal work happens for-real inside
  Linux, matching the go-to-definition/find-references bar CLion set.

## Consequences

Nothing here is exotic or high-risk; the interesting engineering is in the
feed handling and order book, not the tooling. Revisit serialization once
there's a concrete wire format need (journaling, IPC, or persisted book
snapshots) rather than choosing one speculatively now.

## Update (2026-09-12): C++20 modules de-scoped for now

Confirmed Clang 21 genuinely supports C++20 named modules end-to-end —
CMake's `FILE_SET CXX_MODULES` + Ninja built and ran a real
`export module`/`import` smoke test with no issues. However, clangd's
modules support is explicitly experimental (`--experimental-modules-support`)
and, in practice, cross-translation-unit navigation didn't work even with
that flag enabled: Find All References / Go to Definition stayed scoped to
whichever single file was open, never crossing the `import` boundary.

Since reliable navigation is an explicit, stated priority for this project
(it's the specific gap this whole VS Code/clangd setup was meant to close
relative to CLion), modules are **de-scoped from the actual codebase for
now** — back to a plain `.h`/`.cpp` split. This is a tooling-immaturity
call, not a language-support problem: the compiler and build system both
handle modules correctly today. Revisit once clangd's modules support
matures past experimental.
