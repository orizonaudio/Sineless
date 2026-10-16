#!/usr/bin/env bash
# Runs the libFuzzer targets side by side for a fixed time (Linux, Clang). Build them first:
#   cmake --preset fuzz && cmake --build --preset fuzz
# Usage: scripts/fuzz.sh [seconds, default 60] [fuzzer names, default all]
#        scripts/fuzz.sh --prune [fuzzer names, default all]
#
# New inputs that reach new code are kept in build/fuzz/corpus/<fuzzer>, so later runs start from
# where earlier ones got to; CI keeps that corpus between runs (the fuzz job and fuzz-nightly.yml).
# --prune shrinks it to the fewest inputs that reach the same code (libFuzzer's -merge=1), as the
# nightly run does before keeping it. A crash, a sanitizer report or a failed invariant stops that
# fuzzer and saves the input as build/fuzz/artifacts/<fuzzer>-crash-<hash>; reproduce it with
#   build/fuzz/engine/tests/fuzz/<fuzzer> build/fuzz/artifacts/<fuzzer>-crash-<hash>
set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build/fuzz"
bin="$build/engine/tests/fuzz"
prune=false
if [ "${1:-}" = --prune ]; then
    prune=true
    shift
    seconds=0
else
    seconds="${1:-60}"
    shift || true
fi
fuzzers=("$@")
if [ ${#fuzzers[@]} -eq 0 ]; then
    fuzzers=(state_fuzzer setting_json_fuzzer session_fuzzer)
fi

if $prune; then
    mkdir -p "$build/logs"
    for fuzzer in "${fuzzers[@]}"; do
        corpus="$build/corpus/$fuzzer"
        [ -d "$corpus" ] || continue
        pruned="$build/corpus/$fuzzer.pruned"
        rm -rf "$pruned"
        mkdir -p "$pruned"
        "$bin/$fuzzer" -merge=1 -max_len=4096 -rss_limit_mb=2048 -timeout=10 "$pruned" "$corpus" \
            >"$build/logs/$fuzzer-prune.log" 2>&1 || {
            echo "Pruning $fuzzer failed:" >&2
            tail -n 40 "$build/logs/$fuzzer-prune.log" >&2
            exit 1
        }
        echo "== $fuzzer: $(find "$corpus" -type f | wc -l) inputs pruned to $(find "$pruned" -type f | wc -l)"
        rm -rf "$corpus"
        mv "$pruned" "$corpus"
    done
    exit 0
fi

# How long and on how many cores each fuzzer runs. The state and setting fuzzers run thousands of
# inputs a second and stopped finding new code within their first minute on CI, so they get at
# most a minute and one core each. The session fuzzer runs a few dozen a second (each input builds
# two engines and an analyzer) and still finds new code, so it gets the whole run and the other
# cores (libFuzzer's fork mode).
cores="$(nproc 2> /dev/null || sysctl -n hw.ncpu)"
budget() {
    case "$1" in
    session_fuzzer)
        echo "-max_total_time=$seconds -fork=$((cores > 2 ? cores - 1 : 1))"
        ;;
    *)
        echo "-max_total_time=$((seconds < 60 ? seconds : 60))"
        ;;
    esac
}

# When a crash the fuzzers found is fixed, its input goes in
# engine/tests/fuzz/regressions/<fuzzer>/ with the fix, and every run replays those first, so the
# crash cannot come back unnoticed. A crash not fixed yet is an issue, not a file here.
regressions="$root/engine/tests/fuzz/regressions"
shopt -s nullglob
for fuzzer in "${fuzzers[@]}"; do
    inputs=("$regressions/$fuzzer"/*)
    if [ ${#inputs[@]} -gt 0 ]; then
        echo "Replaying ${#inputs[@]} fixed crash input(s) for $fuzzer"
        "$bin/$fuzzer" "${inputs[@]}" || exit 1
    fi
done
shopt -u nullglob

mkdir -p "$build/artifacts" "$build/logs"
pids=()
for fuzzer in "${fuzzers[@]}"; do
    corpus="$build/corpus/$fuzzer"
    mkdir -p "$corpus"
    # -rss_limit_mb: an input that makes the engine allocate without bound is a bug too.
    # shellcheck disable=SC2046 # budget's options are separate words.
    "$bin/$fuzzer" "$corpus" "$bin/seeds/${fuzzer%_fuzzer}" \
        -dict="$bin/fuzz.dict" \
        $(budget "$fuzzer") \
        -max_len=4096 \
        -rss_limit_mb=2048 \
        -timeout=10 \
        -print_final_stats=1 \
        -artifact_prefix="$build/artifacts/$fuzzer-" \
        >"$build/logs/$fuzzer.log" 2>&1 &
    pids+=($!)
done
echo "Fuzzing ${fuzzers[*]} for up to ${seconds}s"

failed=()
for i in "${!fuzzers[@]}"; do
    fuzzer="${fuzzers[$i]}"
    if wait "${pids[$i]}"; then
        echo "== $fuzzer: no failure"
        # A single process's DONE line and final stats, or fork mode's last progress report.
        grep -E '^(#[0-9]+[[:space:]]+DONE|stat::)' "$build/logs/$fuzzer.log"
        grep -E '^#[0-9]+: cov:' "$build/logs/$fuzzer.log" | tail -n 1
    else
        failed+=("$fuzzer")
        echo "== $fuzzer: FAILED"
        tail -n 80 "$build/logs/$fuzzer.log"
    fi
done

if [ ${#failed[@]} -gt 0 ]; then
    echo "Fuzzing found a failure in: ${failed[*]} (inputs in $build/artifacts)" >&2
    exit 1
fi
