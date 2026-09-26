---
status: open
component: quality gates, release process
blocking: false
raised-by: ADR cleanup, 2026-09-23 (was tier 3 of decisions/0005)
assumed: no release checks; dev goes into main when the owner asks (docs/tasks/pull-requests.md)
---

# What checks run before a release, and what counts as untested code?

The branching question is settled: feature branches are squash-merged into `dev`,
and `dev` goes into `main` as a merge commit for a tagged release. What a release
must pass is not:

- **Simulation runs, and build-time and runtime performance regressions**: run
  on the `dev` -> `main` PR, or on a schedule? Against which baseline?
- **Coverage**: the `coverage` preset instruments builds, but nothing reads its
  output (no gcovr or llvm-cov report, no diff-coverage check). "Don't add
  untested code" needs a tool and a threshold or a diff-coverage rule.
- **Who runs it**: a "release manager" role was floated, not designed.
- **CI**: every check today is a local hook. A required CI check is what makes
  any of them unbypassable.

Related: the `in-dev` label and generating the release PR's `Closes #N` list
and changelog from it (worked on in another session).
