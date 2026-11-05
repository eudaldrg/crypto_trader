# Architecture

How the pieces fit and the design choices that are cheap to change. The
expensive-to-reverse decisions are ADRs in `decisions/`; open questions are one
file each in `docs/questions/`; how each component works is in `docs/modules/`.
This file is meant to change as the project learns.

## Execution modes

The pipeline is built for three modes. Today capture with live books exists, and
replay of one connection's journal through the same books; strategies, replay
in live order across connections, and Simulation do not:

1. **LiveTrading**: real exchange connections feed order books that strategies
   read. These books apply the exchange's diffs and never match; self-match
   prevention, if any, is a strategy concern.
2. **Replay**: a captured session replayed deterministically for debugging: if N
   exchanges fed M strategies live, each strategy gets the same callbacks in the
   same order on replay.
3. **Simulation**: a fake exchange replays captures in best-approximated real
   order, keeps *matching* books, and takes orders from the same order-entry hook
   that sends real orders live and is a no-op (or a digest) in Replay.

Strategy-facing code is meant to be identical across the three; only the code
nearest the exchange differs (`decisions/0004`).

## Components

```
exchange ─► client (connection thread) ─► CaptureSession ─┬─► journal ring ─► journal thread ─► file
                                                           └─► book ring ─► book thread ─► BookAdapter ─► books
journal file ─► journal_replay ─► BookAdapter ─► books          (inline, no ring)
```

- `src/feed_handler/`: clients, capture session, journal, config, the `feed_handler`
  binary. [feed-handler](modules/feed-handler.md), [kraken-client](modules/kraken-client.md),
  [deribit-fix](modules/deribit-fix.md).
- `src/order_book/`: the golden books. [order-book](modules/order-book.md).
- `src/book_adapter/`: frames to books, live and replay (`decisions/0008`).

## Capture process and configuration

- **One process runs every connection**, across exchanges. The two per-exchange
  binaries it replaced were about 60% identical, and the epoll-group target
  (`decisions/0010`) and cross-feed consumers both want the connections in one
  address space.
- **Configuration is a TOML file**, one `[[connections]]` entry per socket and
  journal. TOML because its array-of-tables is exactly "a list of connections",
  its scalars are unambiguous and it allows comments. Schema:
  [feed-handler](modules/feed-handler.md#schema).
- **Capture scope target: about the top five symbols per exchange**, one
  connection each, raw and uncompressed, about 6 GiB a day. Measurements and the
  alternatives weighed are in
  `docs/investigations/2026-09-20-capture-volume.md`. Re-measure once it runs;
  per-symbol volume varies widely.

## Toolchain and dependencies

Clang only, pinned to version 21, with `lld`, CMake presets and Ninja
(`scripts/bootstrap.sh`, AGENTS.md). Every dependency comes in through
`FetchContent`, pinned to a tag and marked `SYSTEM`; each is replaceable behind
the one place that uses it, so none of these is an ADR. Adding one means a
pinned `FetchContent_Declare` plus a line here saying what it is for.

| Dependency | For | Notes |
|---|---|---|
| IXWebSocket | Kraken WSS transport | Chosen over Boost.Beast: no Boost anywhere else, and Asio is heavy for one client. Owns its socket and thread, which keeps Kraken out of an epoll group (`decisions/0010`); a hand-rolled RFC 6455 client is the planned replacement, as an exercise like the FIX session layer |
| simdjson | Kraken client: message classification and REST responses | On-demand parsing; the dev CPU (i5-12400F) has AVX2 for its fast kernel |
| nlohmann_json | Kraken `level3` book parsing (`kraken_l3_wire.h`), tests and tools | On the live book thread, so it is in the per-update path; moving it to simdjson (or benchmarking Glaze) is a profiling decision |
| toml++ | the capture config | Header-only |
| moodycamel `readerwriterqueue` | the SPSC rings (`spsc_ring.h`) | Header-only; no multi-cursor ring (`docs/questions/2026-09-23-ring-copies-and-slots.md`) |
| zlib | journal CRC-32 | Already linked through IXWebSocket |
| GoogleTest | every test binary (`add_project_test`) | Chosen over Boost.Test, which drags in the Boost ecosystem |

Deliberately not used:

- **Abseil / ankerl** hash and btree maps: candidates for the fast books; the
  golden book stays on `std::map` (`decisions/0013`).
- **QuickFIX**: the FIX layer is hand-rolled on purpose (`decisions/0001`).
- **Boost.Serialization**: versioning trouble in past use. No serialization
  library is needed while the journal stores raw wire bytes (`decisions/0009`).
- **C++20 modules** (`decisions/0002`).

## Stretch goals

- **Deribit SBE over UDP multicast**, the closest available analog to an
  institutional colocated feed. Free, but needs Equinix LD4 colocation or an AWS
  Transit Gateway peering request in eu-west-2 or ap-northeast-1, with real
  infrastructure cost. Worth it once the pipeline (capture, books, a strategy)
  works end to end on the current feeds (`decisions/0001`).
