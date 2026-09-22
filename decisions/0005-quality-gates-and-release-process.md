# 0005. Quality gates: what's enforced when, and what's still open

## Context

Building the v1 feed handler (`decisions/0004`) surfaced several real bugs
that automated tooling should have caught before they needed a manual
independent review to find:

- A dangling-`std::string_view` pattern across ~20+ test call sites
  (`ParseMessage(SomeBuilder(...))`, where the builder's return value is a
  temporary destroyed before the parsed result is used) that reproduced as a
  genuine heap-use-after-free — but only once sanitizers actually ran.
- `ENABLE_ASAN`/`ENABLE_UBSAN` had been silently inert since the initial
  scaffold: the flags were set on a `project_sanitizers` interface library
  that nothing ever linked into an actual target. Turning the option on
  built and ran clean, that was a false negative.
- A lost-wakeup race (`notify_all()` called without holding the mutex a
  waiter's predicate is checked under) across both exchange clients — exactly
  the class of bug a thread sanitizer exists to catch, and nothing had ever
  run one against this codebase.

None of these needed exotic tooling to catch — they needed the tooling this
project already has (`decisions/0002`'s sanitizer choice) actually wired up
and actually run on a schedule, rather than as an occasional manual check.
This ADR is about closing that gap, and about being explicit that a real
release process is a separate, harder problem this project isn't ready to
commit to yet.

## Decision

Three enforcement tiers, matching how expensive each check is and how often
it should run. Only the first two are implemented; the third is deliberately
left open (see Consequences).

### Tier 1 — commit-time: it must compile, and pass lint

Enforced via the existing `pre-commit` hook (default git-hook stage,
`.pre-commit-config.yaml`):

- `clang-format`, `clang-tidy`, `cppcheck` (already existed).
- **New**: `scripts/check_build.sh` — an actual incremental
  `cmake --build build/debug`. clang-tidy's own analysis pass catches most
  compile errors in a *changed* file as a side effect, but not a link error
  or a break in a file the commit didn't touch directly, so this is a real
  build rather than relying on that side effect.

Deliberately cheap: this runs on every commit, so it stays fast (incremental,
one build directory, no sanitizers, no full test run).

### Tier 2 — push-time: the full test suite, under every sanitizer

Enforced via a **new pre-push hook stage** (same `pre-commit` framework, a
different git hook — install with
`pre-commit install --hook-type pre-push` in addition to the default
`pre-commit install`):

- `scripts/run_sanitizers.sh` builds and runs `feed_handler_tests` under two
  separate configurations, each in its own build directory since they can't
  share a binary:
  - **ASan + UBSan** together (they compose fine in one binary) —
    use-after-free, buffer overflows, undefined behavior generally.
  - **ThreadSanitizer** separately (cannot coexist with ASan in one binary) —
    data races, exactly the class of bug the `notify_all()` fix above was.
- Deliberately builds only `feed_handler_tests`, not the live-capture
  binaries: they have no automated entry point (need live credentials and
  network access), and everything they exercise that matters is already
  reached through the test suite, including the loopback-socket tests that
  drive the real client classes over real threads.

**MemorySanitizer was considered and deliberately deferred, not rejected.**
It catches a class neither ASan, UBSan, nor TSan does (reads of uninitialized
memory), which is a real gap. It needs every linked TU, including the C++
standard library, compiled with `-fsanitize=memory` to avoid a flood of false
positives/negatives from uninstrumented code (MSan's shadow-memory tracking
only propagates through instrumented code) — this project currently links
Clang against GCC's libstdc++ (no `-stdlib=libc++` set anywhere), which has
no blessed MSan-instrumented build path. The realistic route is switching to
LLVM's libc++ (which *does* have documented MSan-instrumented build recipes)
and maintaining a second, MSan-instrumented copy of it alongside the normal
one. Not impossible — a one-time build-script/CMake-preset cost, same shape
as `decisions/0003`'s native-Linux-for-`perf-c2c` deferral — just real
infrastructure work that loses out to higher-priority items right now.
Genuinely want this eventually, not just "if a bug shows up that only MSan
would have caught": revisit once the higher-priority backlog clears, not only
reactively.

**This tier is advisory, not a hard gate, and that's a known limitation.**
A local git hook only runs on a machine that has it installed, and is always
bypassable with `--no-verify`. The only way to make a check actually
unbypassable is server-side enforcement (a CI run required by branch
protection before a merge to `main` can happen at all). There is no CI in
this project yet — that's explicitly future work, not implemented here. The
local hook is still worth having now: it catches things before they leave
this machine, which is strictly better than catching them never, but it is
not the same guarantee a required CI check would be.

### Tier 3 — release-time: branching decided and first mechanism built; simulation/perf/coverage checks still open

The project owner wants, eventually: running simulations, checking build
times don't regress, checking runtime performance doesn't regress, coverage
thresholds, and flagging newly-added code that has no test covering it. None
of that is built yet. Recording the open questions now so they aren't
re-litigated from scratch later, and recording what has since been decided:

- **When does a release check run?** Decided: shape 3 (a parallel branch
  alongside `main`) from the three discussed originally —
  `feature/* -> dev` continuously, `dev -> main` only to cut a release
  (tagged). `dev` is not GitHub's "default branch" (`main` still is), which
  matters below.
- **Tracking what a merge into `dev` closes, before it is actually
  released**: GitHub's own `Closes #N` PR-body syntax only auto-closes an
  issue when the closing PR merges into the *default* branch — merging into
  `dev` does nothing automatically, confirmed empirically (issues #3/#4 sat
  open 31 minutes after their PR merged into `dev`, until closed by hand).
  `.github/workflows/release-tracking.yml` now reads the same `Closes #N` /
  `Fixes #N` / `Resolves #N` wording a PR body already carries (so it is
  written once, not twice) and, on a PR merging into `dev`, adds an `in-dev`
  label to every issue it references instead of closing it — a real "landed
  in dev, not yet released" state that plain GitHub issues (open/closed
  only) cannot express on their own. On the `dev -> main` release PR, the
  same workflow removes that label (GitHub's own mechanism does the actual
  closing there, since `main` is the default branch). The label list at
  release time is also the release-notes input the project owner wanted a
  way to build. `scripts/release_tracking.py` has the extraction regex and
  is unit-testable without a live PR.
- **Coverage / untested-code checks**: still open. Wants "don't add
  untested code" as a gate eventually. Needs a coverage tool decision (the
  `coverage` CMake preset with `--coverage` instrumentation already exists
  per `decisions/0002`/CLAUDE.md, but nothing consumes its output yet — e.g.
  no gcovr/llvm-cov report generation or a diff-coverage threshold check)
  and a decision on what threshold or diff-coverage policy actually means
  "don't add untested code" in practice.
- **A "release manager" role/process** was floated (something that owns
  running simulations, perf comparisons, and deciding a release is healthy)
  but not designed. The branching shape it would depend on is now decided
  (above); what it would actually check (simulations, perf comparisons) is
  still open.

The simulation/perf/coverage checks still block nothing today and remain
speculative until there's a concrete case in front of them. The branching
shape and the `in-dev` tracking above are no longer speculative: they are
the actual mechanism the first real release will use.

## Consequences

- Every `git push` now costs two additional full builds and test runs
  (ASan+UBSan, then TSan) on top of whatever `build/debug` already had
  cached — noticeably slower than before, accepted deliberately given
  tonight's experience of exactly these bug classes going uncaught
  otherwise.
- `scripts/check_build.sh` and `scripts/run_sanitizers.sh` both assume
  `cmake --preset debug` has been run at least once; `run_sanitizers.sh`
  configures its own separate sanitizer build directories on demand, so
  those don't need pre-existing setup the way `build/debug` does.
- Tier 3 remains explicitly unimplemented. Anyone picking this up later
  should read the open questions above before designing something, not
  assume a branching/versioning strategy was already decided.
