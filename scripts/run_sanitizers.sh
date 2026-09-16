#!/usr/bin/env bash
# Builds and runs feed_handler_tests under every sanitizer configuration this
# project uses, each in its own build directory since ASan/UBSan and
# ThreadSanitizer instrument differently and cannot share a binary (see
# decisions/0005-quality-gates-and-release-process.md).
#
# Used by the pre-push hook (.pre-commit-config.yaml) and safe to run by hand:
#   scripts/run_sanitizers.sh
#
# Deliberately builds only feed_handler_tests, not the live-capture binaries:
# the binaries have no automated entry point (they need live credentials and
# network access), and every code path they exercise that matters here is
# already reached through feed_handler_tests, including the loopback-socket
# tests that drive the real client classes over real threads.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

run_variant() {
    local name="$1"
    shift
    local build_dir="build/sanitize-${name}"
    echo "==> ${name}: configuring ${build_dir}"
    cmake --preset debug -B "${build_dir}" "$@" >/dev/null
    echo "==> ${name}: building feed_handler_tests"
    cmake --build "${build_dir}" -j"$(nproc)" --target feed_handler_tests
    echo "==> ${name}: running feed_handler_tests"
    ctest --test-dir "${build_dir}" --output-on-failure
    echo "==> ${name}: clean"
}

run_variant asan-ubsan -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
run_variant tsan -DENABLE_TSAN=ON

echo "==> all sanitizer variants clean"
