---
aliases: [fuzzing, fuzz, libfuzzer, corpus, ENABLE_FUZZER]
sources: [src/feed_handler/fix/fuzz/**]
---

# Fuzzing

Coverage-guided fuzzing with libFuzzer, for bytes nobody wrote a test for. The
sanitizer suite ([testing](testing.md)) only exercises inputs the tests construct;
a hand-rolled parser can pass all of them and still read out of bounds on a
truncated `BodyLength`, a `NumInGroup` that lies, or a malformed `CheckSum`.

## What is fuzzed

One pilot target, `fix_framer_fuzzer` (`src/feed_handler/fix/fuzz/`): the whole
input goes to a fresh `fix::Framer`, then every message the framer hands out goes
through `ParseMessage`, stopping when the framer's sticky error trips. That is
the pipeline a live socket drives, so a bug in either stage is reachable the way
the wire would reach it. There is no oracle beyond "no crash, no sanitizer
report"; correct field values are `fix_message_test.cpp`'s job.

Not fuzzed:

- `BuildMessage`, the outbound path: it only sees this project's own values.
- The Kraken `level3` parser and the journal reader. Deferred until the pilot has
  had a real time-boxed run and shown whether the harness shape generalizes.

## Running it

```bash
cmake --preset debug -DENABLE_FUZZER=ON -B build/fuzz
cmake --build build/fuzz --target fix_framer_fuzzer
cp -r src/feed_handler/fix/fuzz/corpus /tmp/fix-corpus   # a scratch copy
./build/fuzz/bin/fix_framer_fuzzer -max_total_time=60 /tmp/fix-corpus
```

Never point an exploratory run at the tracked `corpus/`: libFuzzer writes every
new interesting input back into the directory it is given. Pick
`-max_total_time` for the occasion; there is no right budget.

## Corpus and regressions

- The seeds in `corpus/` are real SOH-delimited messages generated with the same
  `BuildMessage` calls the tests use: a Heartbeat, a Logon, and two messages in
  one buffer so the framer's multi-message loop is seeded from the start.
- A crash a run finds gets minimized (`-minimize_crash=1`) and the minimized input
  is committed into `corpus/` together with the fix. A fix without its input is
  not verified, and a later refactor can bring the bug back unnoticed.

## Why it is built this way

- **An opt-in option, flags on the one target.** `-fsanitize=fuzzer` supplies
  libFuzzer's own `main`, so it cannot go into the `project_sanitizers` library
  every executable links. `ENABLE_FUZZER` (default off) gates the target, which
  carries `-fsanitize=fuzzer,address` itself; that is why it is a raw
  `add_executable`, the one exception to `add_project_executable`.
- **It compiles `fix_message.cpp` directly** rather than linking `feed_handler`:
  that library's copy has no coverage instrumentation (so no feedback), and its
  `project_sanitizers` could carry a cached `ENABLE_TSAN`, which cannot coexist
  with libFuzzer's ASan. A second fuzz target is the point to factor this into a
  helper, probably with `-fsanitize=fuzzer-no-link` on the libraries.
- **Not in the push hook.** Fuzzing is open-ended; more time finds more, so it
  does not fit a fixed per-push cost. What could join the hook is a fast replay
  of the committed corpus (`-runs=0`), purely as a regression check; exploratory
  runs stay manual or scheduled
  (`docs/questions/2026-09-23-release-checks.md`).
- libFuzzer ships with the pinned Clang 21 compiler-rt: no new dependency.
