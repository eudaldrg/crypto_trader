# 0003. Development environment: WSL2 now, dual-boot Linux later

## Context

Personal Windows desktop (ASUS PRIME B660M-A D4, i5-12400F, 32GB RAM, one
954GB NVMe already full of Windows). No existing Linux box. Several of the
profiling tools this project cares about (`perf c2c` specifically) depend on
hardware PMU access that WSL2 does not reliably expose — Hyper-V's PMU
passthrough to the WSL2 guest is inconsistent and still open as an unresolved
upstream issue, unlike bare-metal Linux.

Storage/compute options considered for both dev and capture:

- **NAS** (UGREEN NASync DXP4800 Plus, Pentium Gold 8505, 8GB RAM, real
  Debian 12 host already running Docker/Servarr/Fava): bare-metal Linux, so
  `perf c2c` has a real shot at working there — but it's live household
  infrastructure with limited RAM already shared across other services;
  turning it into an iterative C++ build/dev sandbox risks destabilizing
  things people rely on daily, for no real compute benefit (message
  rates here are trivial regardless of host).
- **Disposable cloud VPS** (e.g. DigitalOcean, ~$0.036/hr for 2vCPU/4GB,
  billed per-second): real Linux, zero ongoing cost when not running, ideal
  for occasional profiling passes. No capacity/setup baggage.
- **This machine, dual-boot Linux off a dedicated internal NVMe**: chosen —
  see Decision.

## Decision

1. **Bootstrap and do day-to-day development in WSL2** (Ubuntu 26.04 LTS) on
   the existing Windows desktop, right now, for zero-cost/zero-delay start.
   Repo lives on WSL's native ext4 filesystem (`~/projects/crypto_trader`),
   never under `/mnt/c/...` — crossing that boundary (9P protocol) is
   measured at ~7x slower on writes and ~60x slower on metadata operations.
2. **Migrate to a native dual-boot Ubuntu install** on a dedicated internal
   NVMe once purchased: Samsung 990 PRO 1TB, going into the motherboard's
   free M.2_2 slot (PCIe 4.0 x4, same speed class as the existing drive).
   Chosen over an external USB-enclosure NVMe (cheaper and faster once
   internal — no enclosure/bridge-chip cost or USB bottleneck) and over
   1TB-vs-500GB tradeoffs (at 1TB, the DRAM-equipped 990 PRO ended up
   *cheaper* than the DRAM-less alternative considered, WD Black SN7100,
   making DRAM cache a free upgrade rather than a tradeoff). Needs a
   generic aftermarket M.2 heatsink (board only ships one over the primary
   M.2 slot, not M.2_2) if sustained-load thermal throttling turns out to
   actually contaminate profiling runs — not pre-emptively bought, only if
   observed.
3. **Storage split**: the NAS remains the cold/bulk archive (e.g. a year of
   captured tick data); the local NVMe is the active working set for
   speed-testing/replay/profiling runs (e.g. a week of exchange activity),
   so a profiling run is never bottlenecked or confounded by a network
   round-trip to the NAS.

## Consequences

- Full native Linux (real `perf`/`valgrind`/PMU access) isn't available
  until the dual-boot machine exists. Until then, profiling work that
  specifically needs `perf c2c` should go to a disposable cloud VPS rather
  than being attempted under WSL2.
- Editor is VS Code + Remote-WSL now; same workflow carries over unchanged
  once dev moves to the dual-boot Ubuntu install.
