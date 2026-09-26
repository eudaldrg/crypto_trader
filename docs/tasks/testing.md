---
aliases: [testing, tests, ctest, sanitizers, tsan, memory cap, oom, quality gates, pre-commit, pre-push, msan]
sources: [scripts/with_memory_cap.sh, scripts/run_sanitizers.sh, scripts/check_build.sh, .pre-commit-config.yaml]
---

# Testing

How to run the test suites, which automated checks run when, and why every test
runs under a memory cap.

## Run

```bash
ctest --test-dir build/debug                  # every test, each under the cap
scripts/run_sanitizers.sh                     # ASan+UBSan then TSan, what the pre-push hook runs
scripts/with_memory_cap.sh ./build/debug/bin/feed_handler_tests --gtest_filter='JournalThread*'
```

A test binary run directly has no cap; go through `scripts/with_memory_cap.sh`
when running one by hand under a sanitizer.

## The checks

What runs today. The list is the current floor, not a limit: adding a check needs
no ADR, removing or weakening one needs the owner (AGENTS.md).

- **Every commit** (`.pre-commit-config.yaml`, default stage): clang-format,
  clang-tidy, cppcheck, and `scripts/check_build.sh`, an incremental
  `cmake --build build/debug`. The build is there because clang-tidy catches most
  compile errors in a changed file but not a link error or a break in a file the
  commit did not touch. Kept fast: one build directory, no sanitizers, no tests.
- **Every push** (pre-push stage): `scripts/run_sanitizers.sh`, every test binary
  under ASan+UBSan, then again under TSan, each in its own build directory (TSan
  cannot share a binary with ASan). It builds only test binaries: the live
  binaries need credentials and a network, and what they run is reached by the
  loopback-socket tests.
- **Occasionally**: fuzzing ([fuzzing](fuzzing.md)); it is open-ended, so it is
  not a per-push cost.

Both hook stages need installing once per clone (`pre-commit install`, and
`pre-commit install --hook-type pre-push`); a fresh clone has none, see
[pull-requests](pull-requests.md). Both scripts assume `cmake --preset debug` has
run once; the sanitizer build directories configure themselves.

A local hook is advisory, not a gate: it runs only where installed and
`--no-verify` skips it. Only a CI check required by branch protection would be
unbypassable, and there is no CI yet.

These checks exist because of real bugs that went unnoticed until sanitizers
actually ran: a dangling `std::string_view` across about 20 test call sites that
was a genuine heap-use-after-free, `ENABLE_ASAN`/`ENABLE_UBSAN` silently inert
because the flags sat on a library nothing linked, and a lost-wakeup race
(`notify_all()` without the waiter's mutex) in both exchange clients.

**MemorySanitizer is deferred, not rejected.** It catches reads of uninitialized
memory, which nothing else here does, but it needs every linked translation unit,
the C++ standard library included, built with `-fsanitize=memory`. This project
uses GCC's libstdc++, which has no supported MSan build; the route is LLVM's
libc++ plus a second, MSan-instrumented copy of it, a one-time preset and
build-script job. Wanted, not only as a reaction to a bug.

## The memory cap

Every discovered test is launched through `scripts/with_memory_cap.sh` (the
`TEST_LAUNCHER` set in `add_project_test`, `CMakeLists.txt`). It runs the test in
its own systemd scope with `MemoryMax=4G` and swap off, so a test that allocates
without bound is killed on its own and shows up as an ordinary failure.

This matters more than it looks. Every tmux session, so every purplemux tab, is a
systemd scope with `OOMPolicy=stop`. When the kernel OOM-kills any process in it,
systemd stops the whole scope: the shell and the Claude Code session go with it,
and the tab is just gone (purplemux logs `session not found`). Other tabs survive,
because each has its own scope. On 2026-09-21 at 03:07:26 a `feed_handler_tests`
run under TSan reached 12.3 GB and ended a plan run mid-task this way. The kernel
log has the kill (`journalctl -k | grep -i 'out of memory'`).

- `TEST_MEMORY_MAX=2G ctest ...` changes the cap; `0` or `infinity` turns it off.
- The largest suite peaks at about 400 MB under TSan, so 4G leaves room for
  ASan's shadow memory. Raise it for one run rather than changing the default.
- With no systemd user session (a container, another OS) the script runs the test
  uncapped and says so once on stderr. It does not fail.
- A test killed by the cap fails with exit 137 and no gtest output. Rerun it alone
  with a bigger cap, or watch its RSS, before assuming the cap is wrong.

## Writing tests that cannot run away

The test that caused the incident fed 1 MiB payloads into a 2-event ring and
expected the ring to overflow. It did in the debug build, where the consumer is
slow, and never did under TSan. A recording sink kept every payload, so the only
backstop was the loop bound: 20,000 frames, about 20 GB.

- **Force a race outcome, do not hope for it.** To overflow a ring, leave the
  consumer unstarted: `JournalThread::Config::start = false`, or
  `CaptureSession::Config::journal_start = false` through a session. Closing a
  file (`Disconnect`) starts it, so the barrier still works.
- **Bound the loop by bytes, not just by count.** Loop bound times payload size
  should stay in the low megabytes, and use a sink that counts frames when the
  test does not need the payloads back.
- Debug and sanitizer builds change timing by an order of magnitude, so a test
  that passes because one side is slow is a test that is wrong somewhere else.
