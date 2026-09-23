---
aliases: [dev environment, wsl, wsl2, setup, bootstrap, editor, vscode, clangd, dual boot, production machine, storage]
sources: [scripts/bootstrap.sh, CMakePresets.json]
---

# Development environment

Where this project is built and run, and the plan for moving it. Nothing here
constrains the code; it records the machine it runs on.

## Now: WSL2

- **WSL2, Ubuntu 26.04 LTS**, on a Windows desktop (i5-12400F, 32 GB).
  `scripts/bootstrap.sh` sets up a fresh box.
- **The repo lives on WSL's own ext4** (`~/projects/crypto_trader`), never under
  `/mnt/c/...`: crossing into the Windows filesystem (9P) measured about 7x slower
  on writes and 60x slower on metadata operations.
- **Editor: VS Code with Remote-WSL and clangd.** The UI stays native on Windows
  while indexing, building and terminals run in Linux. Navigation through clangd
  is a hard requirement (`decisions/0002`).
- **Profiling that needs `perf c2c` does not work here**: WSL2 does not reliably
  expose the hardware PMU. Use a disposable cloud VPS until the native install
  exists ([profiling](profiling.md)).

## Planned: native Ubuntu, dual-boot

A dedicated internal NVMe (Samsung 990 PRO 1 TB, in the board's free M.2_2 slot,
PCIe 4.0 x4) with a native Ubuntu install, for real `perf`/PMU access. The same
editor workflow carries over. A heatsink for that slot only if thermal
throttling is actually observed contaminating profiling runs.

## Storage

- **Production machine**: cold archive for captured data (e.g. a year of ticks). It is
  not a build or dev host.
- **Local NVMe**: the working set for replay and profiling runs (e.g. a week of
  captures), so a profiling run is never slowed or confounded by the network.
- A large capture kept for profiling lives outside the repo in `~/Temp`, not
  `/tmp` and not in git (AGENTS.md).
