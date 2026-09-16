#!/usr/bin/env bash
# Commit-time gate: the debug preset must actually compile. clang-tidy's own
# analysis pass catches most compile errors in a changed file already, but not
# a link-time error or a break in a file the commit didn't touch directly, so
# this is a real (incremental, fast) build rather than relying on that as a
# side effect. See decisions/0005-quality-gates-and-release-process.md.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [ ! -d build/debug ]; then
    echo "build/debug does not exist -- run 'cmake --preset debug' once first." >&2
    exit 1
fi

cmake --build build/debug
