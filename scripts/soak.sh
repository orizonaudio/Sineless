#!/usr/bin/env bash
# Long-running checks, for releases: the editor playing seven tracks for a while (default 10
# minutes), judged on the rate memory grows at, the slowest minute and the busiest minute; then 200
# editor reopen cycles while the tracks play, judged on memory and on anything left behind.
#
#   scripts/soak.sh [minutes]
#
# Ten minutes shows any leak large enough to matter as a slope. Run 60 by hand after changing
# something long-lived (capture buffers, the musical clock, the page's memory use).
set -euo pipefail
cd "$(dirname "$0")/.."

minutes="${1:-10}"
cmake --preset bench >/dev/null
cmake --build --preset bench -j "$(sysctl -n hw.ncpu)" >/dev/null
bench="build/bench/bin/Release/perf_bench"

echo "==> Soak: $minutes minutes, multi-track, seven tracks over four bars"
"$bench" --mode=multi --lanes=7 --beatspan=fourBars --warmup=10 \
    --duration="$((minutes * 60))" --soak-interval=60 \
    --max-memory-rate=0.5 --min-fps=45 --max-total=65

echo "==> Editor reopened 200 times while playing"
"$bench" --mode=multi --lanes=7 --warmup=5 --reopen=200 --min-fps=45 --max-memory-growth=40
