#!/usr/bin/env bash
# The checks of the code every platform shares, run once, on Linux: the engine, the plugin and the
# editor's shared code under AddressSanitizer with LeakSanitizer, the concurrency tests under
# ThreadSanitizer, the audio thread's tests under RealtimeSanitizer, and clang-tidy. The macOS and
# Windows checks (scripts/check.sh, scripts\check.ps1) cover what only their platform has.
#   scripts/check-linux.sh [asan|tsan|rtsan|lint ...]   (default: all four)
# Needs LLVM 23 (clang-23, clang-tidy-23), CMake, Ninja and Node with node_modules installed.
# Where it does not run on Linux (on macOS), it runs itself in tools/linux-checks/Dockerfile's
# image; the build stays in a Docker volume, so Linux objects never reach the host's build/.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
checks=("$@")
((${#checks[@]})) || checks=(asan tsan rtsan lint)

if [[ "$(uname -s)" != Linux ]]; then
    command -v docker > /dev/null || { echo "Needs Linux or Docker." >&2; exit 1; }
    docker build -q -t sineless-linux-checks tools/linux-checks > /dev/null
    # ThreadSanitizer needs its memory layout without address randomisation: it turns it off for
    # itself, which Docker's default seccomp profile forbids (the personality call).
    exec docker run --rm --security-opt seccomp=unconfined \
        -v "$root":/src -v sineless-linux-build:/src/build -v sineless-linux-modules:/src/node_modules \
        -w /src sineless-linux-checks bash -c 'npm ci --silent && scripts/check-linux.sh "$@"' \
        check-linux "${checks[@]}"
fi

LLVM_BIN="${LLVM_BIN:-/usr/lib/llvm-23/bin}"
# The sanitizers find llvm-symbolizer on the PATH: LLVM 23's, which reads what clang-23 writes.
export PATH="$LLVM_BIN:$PATH"
jobs="$(nproc)"
step() { printf '\n==> %s\n' "$1"; }

sanitized() {
    cmake --preset "$1"
    cmake --build --preset "$1" -j "$jobs"
    ctest --preset "$1" -j "$jobs"
}

for check in "${checks[@]}"; do
    case "$check" in
    asan)
        step "Memory errors and leaks (AddressSanitizer and LeakSanitizer)"
        sanitized linux
        ;;
    tsan)
        step "Concurrency (ThreadSanitizer)"
        sanitized linux-tsan
        ;;
    rtsan)
        step "Real-time safety (RealtimeSanitizer)"
        sanitized linux-rtsan
        ;;
    lint)
        step "C++ lint (clang-tidy)"
        # With the AddressSanitizer build's commands, every shared and Linux source with the SDK,
        # and its generated headers (the protocol's), so it is built first.
        cmake --preset linux
        cmake --build --preset linux -j "$jobs"
        # The platform glue is linted on its own platform (macOS) or built there (Windows). The
        # fuzzers are linted here too: with tests, they build to replay their inputs. One
        # clang-tidy per core, a few files each; xargs fails if any of them does.
        git ls-files -co --exclude-standard -- 'engine/*.cpp' 'editor/*.cpp' 'plugin/*.cpp' \
            ':!editor/src/mac/*' ':!editor/src/win/*' ':!editor/tests/*Win.cpp' |
            xargs -P "$jobs" -n 4 "$LLVM_BIN/clang-tidy" -p build/linux --quiet
        ;;
    *)
        echo "Unknown check: $check (asan, tsan, rtsan or lint)." >&2
        exit 1
        ;;
    esac
done

printf '\nLinux checks passed: %s.\n' "${checks[*]}"
