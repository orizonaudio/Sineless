#!/usr/bin/env bash
# The performance limits, measured on an optimised build of the benchmarks (macOS). Run by
# scripts/check.sh unless SKIP_PERFORMANCE=1, and by CI as a separate job. Two kinds:
#   speed: the audio thread's share of its deadline, and the editor's frames in each scenario of
#     scripts/performance-limits.json: the time between new frames, the share that came late, and
#     the CPU each part spent per frame, judged against the limits set there for this machine
#     (PERF_MACHINE, by default developer-mac) at the display scale the window opened at;
#   leaks: what reopening the editor leaves behind (memory, threads, web views), on any machine.
# PERF_SPEED=report, as CI sets it (its shared machines are slower and noisier than any Mac the
# speed limits are set for, and missed them on every run), measures the speed and reports it
# without judging it; the leak limits are judged everywhere. Each scenario's results are left in
# build/performance/<scenario>.json, in Bencher Metric Format.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

step() { printf '\n==> %s\n' "$1"; }

report=false
[[ "${PERF_SPEED:-}" == report ]] && report=true
machine="${PERF_MACHINE:-developer-mac}"

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
# The editor's frames in each scenario, warmed up until its sweep is full (a 16-bar sweep at
# 60 BPM takes over a minute), then judged together.
results=build/performance
rm -rf "$results"
mkdir -p "$results"
while read -r scenario warmup duration; do
    step "Editor: $scenario"
    if ! "$bench/perf_bench" --scenario="$scenario" --warmup="$warmup" --duration="$duration" \
        --json="$results/$scenario.json" |
        grep -E '^(view|cpu per frame|frames|ticks|page):|^  thread Sineless analysis'; then
        failed=1
    fi
done < <(node scripts/performance-limits.ts scenarios)
step "Editor limits ($machine)"
if ! node scripts/performance-limits.ts judge "$machine" "$results"/*.json && ! $report; then
    failed=1
fi
# Opening and closing the editor leaves nothing behind, and it still draws afterwards (at 45
# frames/s or more; not judged when reporting).
reopenedFps=45
if $report; then reopenedFps=-1; fi
if ! "$bench/perf_bench" --mode=multi --lanes=7 --warmup=5 --reopen=50 --min-fps="$reopenedFps" \
    --max-memory-growth=20 | grep -E '^limit|^reopen'; then
    failed=1
fi

if ((failed)); then
    echo "A performance limit was missed." >&2
    exit 1
fi
