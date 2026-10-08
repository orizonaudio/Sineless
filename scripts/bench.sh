#!/usr/bin/env bash
# Runs the editor performance benchmark over a fixed set of scenarios and prints one summary line
# each: CPU (% of one core) of the analysis thread, the main thread, WebKit and in total, frames
# per second, and data per frame. Build first: cmake --preset bench && cmake --build --preset bench
#
#   scripts/bench.sh [path/to/perf_bench] [seconds per scenario]
set -euo pipefail
cd "$(dirname "$0")/.."

bench="${1:-build/bench/bin/Release/perf_bench}"
duration="${2:-8}"

scenarios=(
    "multi-7-4bars    --mode=multi --lanes=7 --timebase=beats --beatspan=fourBars"
    "multi-7-1bar     --mode=multi --lanes=7 --timebase=beats --beatspan=oneBar"
    "multi-7-roll-4s  --mode=multi --lanes=7 --timebase=seconds --span=4"
    "compare-7-4bars  --mode=compare --lanes=7 --timebase=beats --beatspan=fourBars"
    "single-1bar      --mode=single --timebase=beats --beatspan=oneBar"
    "single-trig-20ms --mode=single --timebase=seconds --span=0.02"
)

printf "%-17s %9s %6s %7s %7s %8s %8s\n" scenario analysis main webkit total "frames/s" "KB/frame"
for scenario in "${scenarios[@]}"; do
    read -r name arguments <<<"$scenario"
    # shellcheck disable=SC2086 # Arguments are split on purpose.
    output="$("$bench" $arguments --warmup=10 --duration="$duration" 2>&1)"
    pick() { grep -E "$1" <<<"$output" | head -1 | sed -E "s|$2|\1|"; }
    analysis="$(pick "thread O-Scope analysis" '.* ([0-9.]+)%.*')"
    main="$(pick "thread main" '.* ([0-9.]+)%.*')"
    webkit="$(pick "^webkit processes" '.* ([0-9.]+)% .*')"
    total="$(pick "^total" '.* ([0-9.]+)% .*')"
    fps="$(pick "frames/s received" 'page: ([0-9.]+) frames.*')"
    size="$(pick "KB/frame" '.*, ([0-9]+) KB/frame')"
    printf "%-17s %9s %6s %7s %7s %8s %8s\n" "$name" "${analysis:-0}" "${main:-0}" "${webkit:-0}" \
        "${total:-0}" "${fps:-0}" "${size:-0}"
done
