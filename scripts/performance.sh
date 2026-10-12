#!/usr/bin/env bash
# The performance limits, measured on an optimised build of the benchmarks (macOS). Run by
# scripts/check.sh unless SKIP_PERFORMANCE=1, and by CI as a separate job: shared CI machines are
# noisier than a developer's Mac, so there the limits are reported rather than enforced.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

step() { printf '\n==> %s\n' "$1"; }

step "Performance limits (optimised benchmark build)"
cmake --preset bench
cmake --build --preset bench -j "$(sysctl -n hw.ncpu)"
bench="build/bench/bin/Release"
# The audio thread: 8 instances together within 5% of every block's deadline at p99.9.
"$bench/process_bench" --instances=8 --seconds=30 --max-share=0.05 | tail -1
# The editor at full rate with seven tracks. CPU in % of one core, about 20 points above what
# an M-series Mac measures: room for noise, not for a real regression.
"$bench/perf_bench" --mode=multi --lanes=7 --beatspan=fourBars --warmup=10 --duration=5 \
    --min-fps=45 --max-analysis=10 --max-total=65 | grep -E '^limit|^total'
"$bench/perf_bench" --mode=compare --lanes=7 --beatspan=fourBars --warmup=10 --duration=5 \
    --min-fps=45 --max-analysis=30 --max-total=75 | grep -E '^limit|^total'
# The worst case: fifteen tracks over sixteen bars at 60 BPM and 96 kHz, a 64 s sweep that
# takes over a minute to fill.
"$bench/perf_bench" --mode=multi --lanes=15 --beatspan=sixteenBars --rate=96000 --tempo=60 \
    --warmup=70 --duration=5 --min-fps=45 --max-analysis=20 --max-total=90 | grep -E '^limit|^total'
# Opening and closing the editor leaves nothing behind.
"$bench/perf_bench" --mode=multi --lanes=7 --warmup=5 --reopen=50 --min-fps=45 \
    --max-memory-growth=20 | grep -E '^limit|^reopen'
