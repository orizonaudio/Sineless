#!/usr/bin/env bash
# The performance limits, measured on an optimised build of the benchmarks (macOS). Run by
# scripts/check.sh unless SKIP_PERFORMANCE=1, and by CI as a separate job. Two kinds:
#   speed: frames/s, CPU and the audio thread's share of its deadline, set for a developer's Mac;
#   leaks: what reopening the editor leaves behind (memory, threads, web views), on any machine.
# PERF_SPEED=report, as CI sets it (its shared machines are slower and noisier than any Mac the
# speed limits are set for, and missed them on every run), measures the speed and reports it
# without judging it; the leak limits are judged everywhere.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

step() { printf '\n==> %s\n' "$1"; }

report=false
[[ "${PERF_SPEED:-}" == report ]] && report=true
# A speed limit, or -1 to measure without judging (perf_bench skips a negative limit).
speed() { if $report; then echo -1; else echo "$1"; fi; }
minFps="$(speed 45)" # frames/s every editor scenario must reach

# Every limit is measured, and the script fails at the end if any was missed.
failed=0

step "Performance limits (optimised benchmark build)"
cmake --preset bench
cmake --build --preset bench -j "$(sysctl -n hw.ncpu)"
bench="build/bench/bin/Release"
# The audio thread: 8 instances together within 5% of every block's deadline at p99.9. Over it,
# process_bench exits 1, which report mode lets pass; any other failure is a crash, never let pass.
"$bench/process_bench" --instances=8 --seconds=30 --max-share=0.05 | tail -1 && status=0 ||
    status="${PIPESTATUS[0]}"
if ((status != 0)) && { ((status != 1)) || ! $report; }; then
    failed=1
fi
# The editor at full rate with seven tracks. CPU in % of one core, about 20 points above what
# an M-series Mac measures: room for noise, not for a real regression.
if ! "$bench/perf_bench" --mode=multi --lanes=7 --beatspan=fourBars --warmup=10 --duration=5 \
    --min-fps="$minFps" --max-analysis="$(speed 10)" --max-total="$(speed 65)" |
    grep -E '^limit|^total|^page:|^  thread Sineless analysis'; then
    failed=1
fi
if ! "$bench/perf_bench" --mode=compare --lanes=7 --beatspan=fourBars --warmup=10 --duration=5 \
    --min-fps="$minFps" --max-analysis="$(speed 30)" --max-total="$(speed 75)" |
    grep -E '^limit|^total|^page:|^  thread Sineless analysis'; then
    failed=1
fi
# The worst case: fifteen tracks over sixteen bars at 60 BPM and 96 kHz, a 64 s sweep that
# takes over a minute to fill.
if ! "$bench/perf_bench" --mode=multi --lanes=15 --beatspan=sixteenBars --rate=96000 --tempo=60 \
    --warmup=70 --duration=5 --min-fps="$minFps" --max-analysis="$(speed 20)" \
    --max-total="$(speed 90)" | grep -E '^limit|^total|^page:|^  thread Sineless analysis'; then
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
