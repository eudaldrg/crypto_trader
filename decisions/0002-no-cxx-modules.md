# 0002. Plain headers, not C++20 modules, while the tooling cannot follow them

- Status: provisional
- Date: 2026-09-23
- Revisit when: clangd resolves Go to Definition and Find References across an
  `import` without experimental flags, and the lint hooks (clang-tidy, cppcheck)
  accept module units.

## Context

The project wanted C++20 named modules. Reliable code navigation is a hard
requirement: it is what the editor setup (VS Code, clangd) was chosen for, and
LLM-assisted work leans on the same navigation. The code is also checked on
every commit by clang-tidy and cppcheck, and build time is traced per
translation unit (`-ftime-trace`, ClangBuildAnalyzer). The choice spreads through
every file: converting later is a rewrite of every header, in either direction.

Verified on 2026-09-12: Clang 21 with CMake `FILE_SET CXX_MODULES` and Ninja
built and ran an `export module`/`import` smoke test with no issues. clangd's
modules support is experimental (`--experimental-modules-support`), and even
with it enabled Find References and Go to Definition did not cross the `import`
boundary.

## Options

### A. C++20 modules throughout

Faster incremental builds and real encapsulation. Hard: navigation stops at
every `import`; the lint hooks' support is unverified (cppcheck is not known to
parse module units, clang-tidy needs the built module interfaces); build traces
gain a module-interface phase to account for.

### B. Plain `.h`/`.cpp`

Everything navigates, lints and traces today. Costs header parsing time and no
language-level encapsulation.

### C. Modules only for leaf libraries

Contains the navigation loss. Hard: two conventions to maintain, and the leaf
libraries are the ones most often read.

## Decision

B, because navigation is a hard requirement and it is the one thing verified
broken; the compiler and build system were fine. The tooling concerns beyond
navigation (lint hooks, build traces) are expected, not verified.

## Consequences

- No `export module`/`import` in project code.
- When the trigger fires, re-run the smoke test through clangd and the commit
  hooks before converting anything.
