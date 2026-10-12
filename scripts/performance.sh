#!/usr/bin/env bash
# The performance limits, measured on an optimised build of the benchmarks (macOS). Run by
# scripts/check.sh unless SKIP_PERFORMANCE=1, and by CI as a separate job: shared CI machines are
# noisier than a developer's Mac, so there the limits are reported rather than enforced.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

step() { printf '\n==> %s\n' "$1"; }

# CI's shared machines are slower and noisier than a developer's Mac, so CI loosens the limits
# with these; scripts/check.sh keeps the defaults.
minFps="${PERF_MIN_FPS:-45}"     # frames/s every editor scenario must reach
cpuSlack="${PERF_CPU_SLACK:-0}"  # points of a core added to every CPU limit
maxShare="${PERF_MAX_SHARE:-0.05}" # share of each block's deadline the audio thread may use

# Every limit is measured, and the script fails at the end if any was missed.
failed=0

step "Performance limits (optimised benchmark build)"
cmake --preset bench
cmake --build --preset bench -j "$(sysctl -n hw.ncpu)"
bench="build/bench/bin/Release"
# The audio thread: 8 instances together within 5% of every block's deadline at p99.9.
if ! "$bench/process_bench" --instances=8 --seconds=30 --max-share="$maxShare" | tail -1; then
    failed=1
fi
# The editor at full rate with seven tracks. CPU in % of one core, about 20 points above what
# an M-series Mac measures: room for noise, not for a real regression.
if ! "$bench/perf_bench" --mode=multi --lanes=7 --beatspan=fourBars --warmup=10 --duration=5 \
    --min-fps="$minFps" --max-analysis=$((10 + cpuSlack)) --max-total=$((65 + cpuSlack)) |
    grep -E '^limit|^total'; then
    failed=1
fi
if ! "$bench/perf_bench" --mode=compare --lanes=7 --beatspan=fourBars --warmup=10 --duration=5 \
    --min-fps="$minFps" --max-analysis=$((30 + cpuSlack)) --max-total=$((75 + cpuSlack)) |
    grep -E '^limit|^total'; then
    failed=1
fi
# The worst case: fifteen tracks over sixteen bars at 60 BPM and 96 kHz, a 64 s sweep that
# takes over a minute to fill.
if ! "$bench/perf_bench" --mode=multi --lanes=15 --beatspan=sixteenBars --rate=96000 --tempo=60 \
    --warmup=70 --duration=5 --min-fps="$minFps" --max-analysis=$((20 + cpuSlack)) \
    --max-total=$((90 + cpuSlack)) | grep -E '^limit|^total'; then
    failed=1
fi
# Opening and closing the editor leaves nothing behind.
if ! "$bench/perf_bench" --mode=multi --lanes=7 --warmup=5 --reopen=50 --min-fps="$minFps" \
    --max-memory-growth=20 | grep -E '^limit|^reopen'; then
    failed=1
fi

if ((failed)); then
    echo "A performance limit was missed." >&2
    exit 1
fi
