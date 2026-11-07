#!/usr/bin/env bash
# The checks of the code every platform shares, run once, on Linux: clang-tidy, the engine, the
# plugin and the editor's shared code under AddressSanitizer with LeakSanitizer, the concurrency
# tests under ThreadSanitizer, and the audio thread's tests under RealtimeSanitizer. The macOS and
# Windows checks (scripts/check.sh, scripts\check.ps1) cover what only their platform has. Every
# check runs, whatever another finds, and the failures are listed at the end (scripts/stages.sh).
#   scripts/check-linux.sh [lint|asan|tsan|rtsan ...]   (default: all four, in that order)
#   CTEST_SHARD=k/n   runs every nth AddressSanitizer test from the kth (ctest -I k,,n), so that n
#                     jobs share them; unset, every test runs
#   BUILD_ONLY=1      builds and runs nothing, to fill the compiler caches
# Needs LLVM 23 (clang-23, clang-tidy-23), CMake, Ninja and Node with node_modules installed.
# Where it does not run on Linux (on macOS), it runs itself in tools/linux-checks/Dockerfile's
# image; the build stays in a Docker volume, so Linux objects never reach the host's build/.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
checks=("$@")
((${#checks[@]})) || checks=(lint asan tsan rtsan)
for check in "${checks[@]}"; do
    case "$check" in
    lint | asan | tsan | rtsan) ;;
    *)
        echo "Unknown check: $check (lint, asan, tsan or rtsan)." >&2
        exit 1
        ;;
    esac
done
shard=()
if [[ -n "${CTEST_SHARD:-}" ]]; then
    if [[ ! "$CTEST_SHARD" =~ ^([1-9][0-9]*)/([1-9][0-9]*)$ ]] ||
        ((BASH_REMATCH[1] > BASH_REMATCH[2])); then
        echo "CTEST_SHARD is k/n with k from 1 to n, as 2/4; it is $CTEST_SHARD." >&2
        exit 1
    fi
    shard=(-I "${BASH_REMATCH[1]},,${BASH_REMATCH[2]}")
fi

if [[ "$(uname -s)" != Linux ]]; then
    command -v docker > /dev/null || { echo "Needs Linux or Docker." >&2; exit 1; }
    docker build -q -t sineless-linux-checks tools/linux-checks > /dev/null
    # ThreadSanitizer needs its memory layout without address randomisation: it turns it off for
    # itself, which Docker's default seccomp profile forbids (the personality call).
    # A worktree's git folder is in the main checkout's, outside this one: mounted read-only where
    # git looks for it, or git ls-files (the lint's file list) fails in the container.
    gitMount=()
    common="$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)"
    if [[ "$common" != "$(cd "$root" && pwd -P)"/* ]]; then
        gitMount=(-v "$common:$common:ro")
    fi
    exec docker run --rm --security-opt seccomp=unconfined \
        -v "$root":/src -v sineless-linux-build:/src/build -v sineless-linux-modules:/src/node_modules \
        ${gitMount[@]+"${gitMount[@]}"} -e BUILD_ONLY -e CTEST_SHARD \
        -w /src sineless-linux-checks bash -c 'npm ci --silent && scripts/check-linux.sh "$@"' \
        check-linux "${checks[@]}"
fi

LLVM_BIN="${LLVM_BIN:-/usr/lib/llvm-23/bin}"
# The sanitizers find llvm-symbolizer on the PATH: LLVM 23's, which reads what clang-23 writes.
export PATH="$LLVM_BIN:$PATH"
jobs="$(nproc)"
source scripts/stages.sh

build() {
    cmake --preset "$1"
    cmake --build --preset "$1" -j "$jobs"
}

lint() {
    # With the AddressSanitizer build's commands, every shared and Linux source with the SDK, and
    # the generated header they include (the protocol's); the other generated headers are written
    # when CMake configures. The platform glue is linted on its own platform (macOS) or built there
    # (Windows). The fuzzers are linted here too: with tests, they build to replay their inputs.
    # One clang-tidy per core, a few files each; xargs fails if any of them does.
    cmake --preset linux
    cmake --build --preset linux --target protocol_header
    git ls-files -co --exclude-standard -- 'engine/*.cpp' 'editor/*.cpp' 'plugin/*.cpp' \
        ':!editor/src/mac/*' ':!editor/src/win/*' ':!editor/tests/*Win.cpp' |
        xargs -P "$jobs" -n 4 "$LLVM_BIN/clang-tidy" -p build/linux --quiet
}

for check in "${checks[@]}"; do
    case "$check" in
    lint)
        # C++ lint (clang-tidy).
        stage "Linux lint" --rerun "scripts/check-linux.sh lint" -- lint
        ;;
    asan)
        # Memory errors and leaks (AddressSanitizer and LeakSanitizer).
        stage "Linux asan build" --build -- build linux
        stage "Linux asan tests" --needs "Linux asan build" --rerun "scripts/check-linux.sh asan" -- \
            ctest --preset linux -j "$jobs" ${shard[@]+"${shard[@]}"}
        ;;
    tsan)
        # Concurrency (ThreadSanitizer).
        stage "Linux tsan build" --build -- build linux-tsan
        stage "Linux tsan tests" --needs "Linux tsan build" --rerun "scripts/check-linux.sh tsan" -- \
            ctest --preset linux-tsan -j "$jobs"
        ;;
    rtsan)
        # Real-time safety (RealtimeSanitizer).
        stage "Linux rtsan build" --build -- build linux-rtsan
        stage "Linux rtsan tests" --needs "Linux rtsan build" --rerun "scripts/check-linux.sh rtsan" -- \
            ctest --preset linux-rtsan -j "$jobs"
        ;;
    esac
done

stages_report
