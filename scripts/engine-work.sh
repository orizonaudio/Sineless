#!/usr/bin/env bash
# The engine's work per frame in every benchmark scenario (engine/bench/AnalysisBench.cpp),
# counted under Valgrind's Cachegrind: the instructions of the measured frames, which are the
# same on every run of one build, so that CI can compare a pull request with its base on shared
# machines (the engine-work jobs in .github/workflows/ci.yml). The worst case also counts its
# last-level cache misses, for the memory its sums read, which instructions do not show.
#
#   scripts/engine-work.sh [out-dir]                 (default: build/engine-work)
#   node scripts/engine-work.ts compare BASE HEAD [AGAIN]
#
# Writes <out-dir>/results.json (Bencher Metric Format) and <out-dir>/scenarios.json, and prints a
# table. Needs Linux with Valgrind 3.22 or later, LLVM 23 (clang-23), CMake, Ninja and Node. Where
# it does not run on Linux (on macOS), it runs itself in tools/linux-checks/Dockerfile's image;
# there it counts an arm64 build, whose counts compare with each other but not with CI's x86-64.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
out="${1:-build/engine-work}"

if [[ "$(uname -s)" != Linux ]]; then
    command -v docker > /dev/null || { echo "Needs Linux or Docker." >&2; exit 1; }
    docker build -q -t sineless-linux-checks tools/linux-checks > /dev/null
    # A build volume of its own for the bench, so that this never shares a build with
    # scripts/check-linux.sh; the results land in the checkout.
    exec docker run --rm \
        -v "$root":/src -v sineless-engine-work-build:/src/build/linux-bench \
        -w /src sineless-linux-checks scripts/engine-work.sh "$out"
fi

# The scenarios whose cache misses are counted too: simulating the caches roughly doubles a run.
cacheScenarios=" compare-15-worst "

step() { printf '\n==> %s\n' "$1"; }

step "Build (linux-bench)"
cmake --preset linux-bench > /dev/null
cmake --build --preset linux-bench -j "$(nproc)"
bench=build/linux-bench/bin/analysis_bench
[[ -x "$bench" ]] || bench="$(find build/linux-bench -type f -name analysis_bench -perm -u+x | head -1)"

rm -rf "$out"
mkdir -p "$out"
valgrind --version > "$out/valgrind.txt"
for scenario in $("$bench" --list | awk '{print $1}'); do
    step "$scenario"
    cacheSim=no
    [[ "$cacheScenarios" == *" $scenario "* ]] && cacheSim=yes
    # Counting starts and stops around the measured frames (the bench's client requests), so the
    # summary is their instructions alone.
    valgrind --tool=cachegrind --cache-sim="$cacheSim" --instr-at-start=no \
        --cachegrind-out-file="$out/$scenario.cachegrind" \
        "$bench" --scenario="$scenario" --json="$out/$scenario.json" 2> "$out/$scenario.log" ||
        { cat "$out/$scenario.log" >&2; echo "Valgrind could not run $scenario." >&2; exit 1; }
done

step "Results"
node scripts/engine-work.ts results "$out"
