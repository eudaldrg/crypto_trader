---
aliases: [testing, tests, ctest, sanitizers, tsan, memory cap, oom]
sources: [scripts/with_memory_cap.sh, scripts/run_sanitizers.sh]
decisions: [decisions/0005-quality-gates-and-release-process.md]
---

# Testing

How to run the test suites and the sanitizer gate, and why every test runs under
a memory cap.

## Run

```bash
ctest --test-dir build/debug                  # every test, each under the cap
scripts/run_sanitizers.sh                     # ASan+UBSan then TSan, what the pre-push hook runs
scripts/with_memory_cap.sh ./build/debug/bin/feed_handler_tests --gtest_filter='JournalThread*'
```

A test binary run directly has no cap; go through `scripts/with_memory_cap.sh`
when running one by hand under a sanitizer. The gate itself and when it runs are
in `decisions/0005`; the hook only exists after `pre-commit install --hook-type
pre-push`, see [pull-requests](pull-requests.md).

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
