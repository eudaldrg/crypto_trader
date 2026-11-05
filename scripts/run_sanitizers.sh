#!/usr/bin/env bash
# Builds and runs every test binary under every sanitizer configuration this
# project uses, each in its own build directory since ASan/UBSan and
# ThreadSanitizer instrument differently and cannot share a binary (see
# docs/tasks/testing.md).
#
# Used by the pre-push hook (.pre-commit-config.yaml) and safe to run by hand:
#   scripts/run_sanitizers.sh
#
# Deliberately builds only the test binaries, not the live-capture binaries:
# the live binaries have no automated entry point (they need live credentials
# and network access), and every code path they exercise that matters here is
# already reached through the test suite, including the loopback-socket tests
# that drive the real client classes over real threads. All test binaries are
# built explicitly (not just one of them) because ctest discovers every
# registered test at configure time regardless of what got built -- leaving
# any of them unbuilt turns into a NOT_BUILT failure, or worse, runs a stale
# binary left in the build directory by an earlier build. The list is read from
# the add_project_test() calls in CMakeLists.txt so it cannot drift (it once
# missed book_adapter_tests, which then ran stale).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

mapfile -t TEST_TARGETS < <(grep -oP '^\s*add_project_test\(\K[A-Za-z0-9_]+' CMakeLists.txt)
if [ "${#TEST_TARGETS[@]}" -eq 0 ]; then
    echo "run_sanitizers.sh: found no add_project_test() targets in CMakeLists.txt" >&2
    exit 1
fi

run_variant() {
    local name="$1"
    shift
    local build_dir="build/sanitize-${name}"
    echo "==> ${name}: configuring ${build_dir}"
    cmake --preset debug -B "${build_dir}" "$@" >/dev/null
    echo "==> ${name}: building test binaries"
    cmake --build "${build_dir}" -j"$(nproc)" --target "${TEST_TARGETS[@]}"
    echo "==> ${name}: running tests"
    ctest --test-dir "${build_dir}" --output-on-failure
    echo "==> ${name}: clean"
}

run_variant asan-ubsan -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
run_variant tsan -DENABLE_TSAN=ON

echo "==> all sanitizer variants clean"
