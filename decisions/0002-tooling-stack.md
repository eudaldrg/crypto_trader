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

## Update (2026-09-16): Networking + JSON parsing, for the feed handler

Two more dependencies needed once real feed-handler work starts
(`decisions/0004-feed-handler-architecture.md`):

- **WebSocket/TLS transport (Kraken)**: start with **IXWebSocket** (small,
  no Boost, TLS via OpenSSL) rather than Boost.Beast — nothing else in this
  stack pulls in Boost, and Beast's Asio dependency is heavy for what's
  needed here. Explicitly **not** the end state, for two reasons: a
  hand-rolled WS client (raw sockets + OpenSSL + hand-written RFC6455
  handshake/framing) is wanted later as a deliberate learning exercise, in
  the same spirit as hand-rolling the Deribit FIX session layer instead of
  using QuickFIX — and, separately, IXWebSocket owns its socket fd
  internally on its own background thread, which blocks a Kraken connection
  from ever joining the epoll-per-thread-group threading model that's the
  actual end-goal there (`decisions/0004`). Tracked as a future replacement,
  not forgotten — swap it in once the IXWebSocket-based pipeline works
  end-to-end and there's a real book to validate the hand-rolled version
  against.
- **JSON parsing (Kraken message bodies)**: **simdjson** — on-demand
  parsing (no full DOM allocation), SIMD-accelerated. Confirmed the dev
  machine's CPU (i5-12400F) has AVX2, which simdjson uses for its primary
  fast kernel, so this isn't a theoretical benefit. **Glaze** (reflection-based
  compile-time (de)serialization) is a genuine alternative worth benchmarking
  later — noted here specifically so it doesn't get forgotten as a follow-up
  comparison once there's real message volume to benchmark against, not
  because simdjson is currently in doubt.
