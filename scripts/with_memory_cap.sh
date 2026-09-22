#!/usr/bin/env bash
# Runs a command under a cgroup memory cap, so a runaway test is killed on its own.
#
# Why: every tmux session (and so every purplemux tab) is a systemd scope with
# OOMPolicy=stop. When the kernel OOM-kills any process in it, systemd stops the
# whole scope: the shell and the Claude Code session go with it. A test that
# allocates without bound (a loop bound times a payload size, say) therefore ends
# the session, not just the test. A cap turns that into an ordinary test failure.
#
# Used as the ctest launcher for every test binary (add_project_test in
# CMakeLists.txt) and safe to use by hand, e.g. around a test binary run directly:
#   scripts/with_memory_cap.sh ./build/debug/bin/feed_handler_tests
#
# TEST_MEMORY_MAX overrides the cap (a systemd size such as 2G or 512M); 0 or
# "infinity" turns it off. The largest suite peaks at about 400 MB under TSan, so the
# default leaves room for ASan's shadow memory. Swap is off so the cap bites at the
# cap, not after the machine has swapped itself to a crawl.
#
# Without a systemd user session (CI containers, macOS) it runs the command
# uncapped and says so once on stderr rather than failing.
set -euo pipefail

cap="${TEST_MEMORY_MAX:-4G}"

if [[ "${cap}" == "0" || "${cap}" == "infinity" ]]; then
    exec "$@"
fi

# The user bus socket is what `systemd-run --user` talks to; checking for it is far
# cheaper than launching a probe, and this runs once per discovered test.
if command -v systemd-run >/dev/null 2>&1 && [[ -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/bus" ]]; then
    exec systemd-run --user --scope --quiet --collect \
        -p MemoryMax="${cap}" -p MemorySwapMax=0 -- "$@"
fi

if [[ -z "${WITH_MEMORY_CAP_WARNED:-}" ]]; then
    echo "with_memory_cap.sh: no systemd user session, running without a memory cap" >&2
    export WITH_MEMORY_CAP_WARNED=1
fi
exec "$@"
