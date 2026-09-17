# 0007. Coverage-guided fuzzing, starting with a FIX pilot

## Context

`decisions/0005` wires ASan+UBSan and TSan into a pre-push gate, but that gate
only exercises the inputs the existing tests happen to construct. It says
nothing about bytes nobody wrote a test for. That gap matters most exactly
where `decisions/0004` puts untrusted external bytes directly against
hand-rolled parsing code: the FIX builder/framer/parser in
`src/feed_handler/fix/` sits on raw bytes read off a live exchange socket, with
no data dictionary or third-party library between the wire and this project's
own field-walking logic (`decisions/0001`, `decisions/0002` — QuickFIX was
deliberately not used; the protocol layer is the exercise). A parser can pass
every test a human thought to write and still read out of bounds on a
truncated `BodyLength`, a `NumInGroup` that lies, or a `CheckSum` field that
isn't three digits. Coverage-guided fuzzing — mutate inputs, keep the ones
that reach new code, let a sanitizer catch what a human oracle never checked
for — is the tool built for exactly that gap.

Clang 21 is already the pinned toolchain (`decisions/0002`) and ships
libFuzzer as part of compiler-rt: `-fsanitize=fuzzer` on a dedicated harness
with an `extern "C" LLVMFuzzerTestOneInput` entry point is the entire
dependency footprint. No new library, no new build system, nothing to
`FetchContent`.

This is deliberately scoped as a **pilot**, one parser only. Two other
obvious candidates — the Kraken `level3` JSON decoder and the v1 journal
reader — are explicitly deferred, not overlooked: proving the harness shape,
the CMake wiring, and the corpus/regression workflow on one target first is
cheaper than debugging all three at once, and the FIX code is the highest-
value target of the three (a hand-rolled byte-level parser, not a
`simdjson`-mediated one, and the one exchange-facing surface where framing
bugs and parsing bugs are both this project's own code end to end).

## Decision

### Target: the `Framer` + `ParseMessage` pipeline, not `BuildMessage`

The harness (`src/feed_handler/fix/fuzz/fix_framer_fuzzer.cpp`) feeds the
entire fuzz input to a fresh `feed_handler::fix::Framer::Append()`, then loops
`NextMessage()` (stopping if `Good()` goes false, matching the sticky-error
contract `decisions/0004` established) and runs `ParseMessage()` on every
message the framer hands back. This is the real pipeline a socket-facing
client drives, not a synthetic slice of it — `Framer` finds message
boundaries from `BodyLength`, `ParseMessage` validates envelope and fields, and
a bug in either stage is reachable exactly the way a live connection would
reach it. `BuildMessage` (the outbound path) is not fuzzed: it only ever
receives this project's own already-valid field values, not exchange-supplied
bytes, so it is not the same class of attack surface.

No oracle beyond "does it crash": correctness of parsed field values is
already `fix_message_test.cpp`'s job (hand-verified checksums, cross-checked
independently in Python per that file's own header comment). Fuzzing exists
to find crashes/UB on inputs nobody enumerated, not to re-verify known-good
behavior.

### CMake wiring: a separate option and a separate sanitizer library

`-fsanitize=fuzzer` requires the binary to supply `LLVMFuzzerTestOneInput` and
must not compete with another `main`. Folding it into the existing
`project_sanitizers` INTERFACE library (gated by `ENABLE_ASAN`/`ENABLE_UBSAN`/
`ENABLE_TSAN`) would attach it to every target linking that library — every
GoogleTest binary and both live-capture executables — and break the link the
moment `ENABLE_FUZZER` was on in the same build directory. So:

- A new option, `ENABLE_FUZZER` (default `OFF`), independent of the sanitizer
  options.
- A new INTERFACE library, `project_fuzzer_sanitizer`, structurally separate
  from `project_sanitizers`, that only adds `-fsanitize=fuzzer,address` (as
  both compile and link options) when `ENABLE_FUZZER` is on.
- The fuzz target itself, `fix_framer_fuzzer`, gated behind
  `if(ENABLE_FUZZER)` the same way test targets are gated behind
  `BUILD_TESTING`. It links `project_options project_warnings
  project_fuzzer_sanitizer` — not `project_sanitizers`, and not the
  `feed_handler` library target. `feed_handler` is `PUBLIC project_sanitizers`,
  which would transitively pull in whatever `ENABLE_ASAN`/`ENABLE_TSAN` happen
  to be cached in that same build directory and collide with the fuzzer's own
  `-fsanitize=address`. Instead, `fix_message.cpp` is compiled directly into
  the fuzz executable with its own
  `target_include_directories(... PRIVATE ${CMAKE_SOURCE_DIR}/src)` — the
  pilot stays self-contained and the rest of the build is untouched.

### Corpus and regression model

Seed inputs live in `src/feed_handler/fix/fuzz/corpus/` as real SOH-delimited
wire bytes (generated from the same `BuildMessage` calls the tests use, not
hand-typed) — a Heartbeat, a Logon, and two whole messages concatenated in one
buffer, to seed the framer's multi-message loop from the start rather than
making libFuzzer rediscover message boundaries from nothing. Any crash a
future run finds gets minimized (`-minimize_crash=1` or libFuzzer's own
offer) and the minimized input is committed into `corpus/` alongside the fix
that resolves it — the corpus is the regression suite for this class of bug,
the same way a captured wire session is the regression fixture for the order
book work. A fix without a committed input is not verified; a future
refactor can silently reintroduce the same bug.

### Cadence: different from Tier 1/2, not wired into the push gate yet

This is **not** added to `scripts/run_sanitizers.sh` in this change. Tier 1/2
(`decisions/0005`) are cheap-relative-to-value, deterministic, fixed-duration
checks that make sense on every commit/push. A real fuzzing run is
open-ended by nature — more time finds more bugs, there is no principled
"done" — so it does not fit that cadence without first deciding a time
budget, which corpus lives where in CI (if any), and whether crashes block a
push or just get filed. None of that is decided here. What could reasonably
be added to the push gate later is a *fast, non-exploratory* corpus-replay
step (`-runs=0` against the checked-in corpus, seconds not minutes) purely to
catch a regression against an already-known input; an actual time-boxed
exploratory fuzzing run stays a manual or periodic activity, run locally or
in a scheduled job, not a per-push cost.

### Running it

```bash
cmake --preset debug -DENABLE_FUZZER=ON -B build/fuzz
cmake --build build/fuzz --target fix_framer_fuzzer
./build/fuzz/bin/fix_framer_fuzzer -max_total_time=60 src/feed_handler/fix/fuzz/corpus/
```

(Executable output path matches every other target in this project —
`CMAKE_RUNTIME_OUTPUT_DIRECTORY` is `<build dir>/bin`, the same place
`kraken_feed_handler` and `feed_handler_tests` land.) Point `-max_total_time`
at whatever budget the moment calls for; there is no fixed number that is
"correct" the way there is for the pre-push sanitizer suite.

## Consequences

- Default builds (`ENABLE_FUZZER` unset/`OFF`) are unaffected: no new option
  changes behavior, no new target is even defined, and `feed_handler_tests`
  and the rest of the existing targets build exactly as before.
- A separate build directory (`build/fuzz` above) is required to actually run
  this, the same way `run_sanitizers.sh` already needs separate ASan+UBSan and
  TSan directories — `-fsanitize=fuzzer,address` cannot share a build
  directory with an ordinary or TSan'd one.
- Kraken `level3` and the journal reader remain unfuzzed. Revisit once this
  pilot has actually found (or failed to find, over a real time-boxed run —
  not just the smoke-test run in this change) something, to decide whether
  the harness shape here generalizes cleanly or needs rethinking per target.
- The corpus will grow over time as crashes are found and minimized; if it
  ever grows large enough to make `git` unhappy about binary blobs, that's a
  problem for whenever it actually happens, not something to design around
  now.
