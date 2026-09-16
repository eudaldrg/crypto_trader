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
memory), which is a real gap. It needs an MSan-instrumented C++ standard
library to be usable at all — running it against the system's ordinary
libstdc++ produces a flood of false positives from the standard library's own
uninstrumented internals, not a small toolchain flag flip. Same shape of
deferral as `decisions/0003`'s native-Linux-for-`perf-c2c` — real value,
real infrastructure cost, not worth blocking on right now. Revisit if a bug
class shows up that only MSan would have caught.

**This tier is advisory, not a hard gate, and that's a known limitation.**
A local git hook only runs on a machine that has it installed, and is always
bypassable with `--no-verify`. The only way to make a check actually
unbypassable is server-side enforcement (a CI run required by branch
protection before a merge to `main` can happen at all). There is no CI in
this project yet — that's explicitly future work, not implemented here. The
local hook is still worth having now: it catches things before they leave
this machine, which is strictly better than catching them never, but it is
not the same guarantee a required CI check would be.

### Tier 3 — release-time: not implemented, open questions recorded here

The project owner wants, eventually: running simulations, checking build
times don't regress, checking runtime performance doesn't regress, coverage
thresholds, and flagging newly-added code that has no test covering it. None
of this is built yet, and neither is a "release" concept at all — there is
nothing to release. Recording the open questions now so they aren't
re-litigated from scratch later:

- **When does a release check run?** Three shapes were discussed, none
  chosen:
  1. After every task/PR — thorough, but likely too slow and too frequent
     for checks like simulations or perf comparisons to make sense at that
     granularity.
  2. Feature branches stay unmerged until a release is being cut — keeps
     `main` always release-ready, but risks a pile of long-lived unmerged
     branches with growing merge-conflict exposure the longer they sit.
  3. A parallel "next release" branch alongside `main` — decouples release
     cadence from merge cadence, but is another branch to keep in sync and
     makes versioning less obviously linear.
- **Coverage / untested-code checks**: wants "don't add untested code" as a
  gate eventually. Needs a coverage tool decision (the `coverage` CMake
  preset with `--coverage` instrumentation already exists per
  `decisions/0002`/CLAUDE.md, but nothing consumes its output yet — e.g. no
  gcovr/llvm-cov report generation or a diff-coverage threshold check) and a
  decision on what threshold or diff-coverage policy actually means "don't
  add untested code" in practice.
- **A "release manager" role/process** was floated (something that owns
  running simulations, perf comparisons, and deciding a release is healthy)
  but not designed — depends on which of the branching shapes above gets
  picked, since that determines what a release manager would actually be
  looking at.

None of tier 3 blocks anything today. Revisit this ADR (not a new one, this
section) once there's an actual reason to cut a release — a decision made
speculatively now, before the project needs one, is more likely to be wrong
than one made when there's a concrete case in front of it.

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
