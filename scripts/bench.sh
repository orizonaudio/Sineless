#!/usr/bin/env bash
# Runs the editor performance benchmark over a fixed set of scenarios and prints one summary line
# each: CPU (% of one core) of the analysis thread, the main thread, WebKit and in total, frames
# per second, resident memory and data per frame. Scenarios play at 48 kHz and 120 BPM unless
# named otherwise. Build first: cmake --preset bench && cmake --build --preset bench
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
    "multi-7-4bars-60bpm-96k --mode=multi --lanes=7 --timebase=beats --beatspan=fourBars --rate=96000 --tempo=60"
    # The worst case: fifteen tracks over sixteen bars at 60 BPM and 96 kHz, a 64 s sweep, which
    # takes over a minute to fill.
    "multi-15-16bars-60bpm-96k --mode=multi --lanes=15 --timebase=beats --beatspan=sixteenBars --rate=96000 --tempo=60 --warmup=70"
    "compare-15-16bars-60bpm-96k --mode=compare --lanes=15 --timebase=beats --beatspan=sixteenBars --rate=96000 --tempo=60 --warmup=70"
    # The same with every track checked and their matches moving, as with real material, with the
    # fix previewed: the sum is redone as matches move.
    "compare-15-all-jitter-96k --mode=compare --lanes=15 --timebase=beats --beatspan=sixteenBars --rate=96000 --tempo=60 --warmup=70 --compare-all --jitter --preview-fix"
    "single-16bars-60bpm-96k --mode=single --timebase=beats --beatspan=sixteenBars --rate=96000 --tempo=60 --warmup=70"
    "compare-15-roll-4s-96k --mode=compare --lanes=15 --timebase=seconds --span=4 --rate=96000"
)

printf "%-25s %9s %6s %7s %7s %8s %8s %8s\n" scenario analysis main webkit total "frames/s" "MB" \
    "KB/frame"
for scenario in "${scenarios[@]}"; do
    read -r name arguments <<<"$scenario"
    # shellcheck disable=SC2086 # Arguments are split on purpose.
    output="$("$bench" --warmup=10 --duration="$duration" $arguments 2>&1)"
    pick() { grep -E "$1" <<<"$output" | head -1 | sed -E "s|$2|\1|"; }
    analysis="$(pick "thread O-Scope analysis" '.* ([0-9.]+)%.*')"
    main="$(pick "thread main" '.* ([0-9.]+)%.*')"
    webkit="$(pick "^webkit processes" '.* ([0-9.]+)% .*')"
    total="$(pick "^total" '.* ([0-9.]+)% .*')"
    fps="$(pick "frames/s received" 'page: ([0-9.]+) frames.*')"
    memory="$(pick "MB resident" '.*, ([0-9]+) MB resident.*')"
    size="$(pick "KB/frame" '.*, ([0-9]+) KB/frame')"
    printf "%-25s %9s %6s %7s %7s %8s %8s %8s\n" "$name" "${analysis:-0}" "${main:-0}" \
        "${webkit:-0}" "${total:-0}" "${fps:-0}" "${memory:-0}" "${size:-0}"
done

# How soon a zoom or a reset shows in the worst case, when every lane's whole span is reduced
# again: from the change to the first frame of the new view.
echo
"$bench" --mode=multi --lanes=15 --timebase=beats --beatspan=sixteenBars --rate=96000 --tempo=60 \
    --warmup=70 --view-changes=30 2>&1 | grep "^view changes"
