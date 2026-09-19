#!/usr/bin/env bash
# Builds and runs every test binary under every sanitizer configuration this
# project uses, each in its own build directory since ASan/UBSan and
# ThreadSanitizer instrument differently and cannot share a binary (see
# decisions/0005-quality-gates-and-release-process.md).
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
# any of them unbuilt turns into a spurious NOT_BUILT ctest failure, not a
# skip. Keep this list in sync with add_project_test()/add_project_executable()
# calls in CMakeLists.txt whenever a new test binary is added.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

TEST_TARGETS=(feed_handler_tests dependency_smoke_test order_book_tests)

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
