---
aliases: [profiling, perf, perf c2c, valgrind, gperftools, tcmalloc, build time, time-trace]
sources: [CMakePresets.json, src/book_adapter/journal_replay.*, src/book_adapter/tools/journal_replay.cpp]
---

# Profiling

What is wired up for measuring runtime and build time, and where each tool can
actually run.

## Runtime

- **`journal_replay`** (`docs/modules/order-book.md`) is the harness for the book
  code: it replays a long capture through the real adapter and reports parse and
  apply time separately from reading. Profile on a long capture kept outside the
  repo (`~/Temp`), not the fixtures.
- **gperftools CPU profiler**: the `profile` preset (release-equivalent) links it
  into every executable (`ENABLE_GPERFTOOLS`).
- **tcmalloc**: `-DENABLE_TCMALLOC=ON`.
- **perf**, including `perf c2c` for cache-line contention between threads, and
  **Valgrind**. All of these are Linux-only, which is why development is on
  Linux at all.

`perf c2c` needs hardware PMU access, which WSL2 does not reliably expose
(Hyper-V's PMU passthrough is inconsistent). Until the native Linux install
exists ([dev-environment](dev-environment.md)), run anything that needs it on a
disposable cloud VPS (for example a 2 vCPU / 4 GB droplet billed per second),
not under WSL2.

## Build time

Every build emits a per-translation-unit `-ftime-trace` JSON next to each object
file, for ClangBuildAnalyzer.
