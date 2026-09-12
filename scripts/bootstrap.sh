#!/usr/bin/env bash
# Sets up a fresh Ubuntu box (WSL2 or bare-metal) to build and develop
# crypto_trader. Safe to re-run. See decisions/0002-tooling-stack.md for why
# each of these was chosen.
set -euo pipefail

sudo apt-get update
sudo apt-get install -y \
    build-essential \
    clang \
    clang-tools \
    clangd \
    clang-format-21 \
    clang-tidy-21 \
    cppcheck \
    pre-commit \
    lld \
    cmake \
    ninja-build \
    git \
    gh \
    pkg-config \
    python3-venv \
    python3-pip

# Point the version-less clang-format/clang-tidy names at the -21 binaries
# (matching the installed clang toolchain) so the pre-commit config and any
# editor integration can just call `clang-format`/`clang-tidy` directly.
sudo update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-21 100
sudo update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-21 100

# Abseil: prefer the distro package if present; otherwise it's pulled via
# CMake FetchContent in the top-level CMakeLists.txt (not yet written).
sudo apt-get install -y libabsl-dev || true

# GoogleTest: Ubuntu's libgtest-dev ships sources, not prebuilt libs, and is
# unreliable to link against directly — fetched via CMake FetchContent
# instead once CMakeLists.txt exists, rather than installed here.

# ankerl::unordered_dense and moodycamel concurrentqueue/readerwriterqueue
# are both single-header libraries with no apt package — vendored or pulled
# via CMake FetchContent once the build is set up, not installed here.

echo "Toolchain versions:"
clang --version
cmake --version
ninja --version
git --version
clang-format --version
clang-tidy --version
cppcheck --version
pre-commit --version
