#!/usr/bin/env bash
# Mutation testing of the C++ engine: Mull builds engine_tests with every mutant compiled in
# (tools/mutation/mull.yml) and runs the tests once per mutant, each run stopping at the first test
# that fails. A mutant no test fails on marks code that no test checks. With a base revision, only
# the engine lines changed since it are mutated, as on a pull request (and nothing is built when
# none changed); with --file, only the files named (an engine source or header each, repeatable);
# with neither, all of engine/src and engine/include, about nine hours:
#   scripts/mutation-cpp.sh [--file engine/src/Analyzer.cpp ...] [base revision, e.g. origin/main]
# Mull needs the Clang it was built for: Ubuntu 24.04's Clang 18, as on CI. Where mull-runner-18 is
# not installed (on macOS), the script runs itself in tools/mutation/Dockerfile's image.
# A report, not a check: writes build/mutation-cpp-report/ (mull.json and summary.md, which also
# says how long the run took) and exits 0 whatever the tests do not notice.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/build/mutation-cpp-report"
cd "$root"
# Passed on to the run in Docker, as ${args[@]+...}: macOS's bash 3.2 calls an empty array
# unbound under set -u.
args=("$@")
base=""
files=()
while (($#)); do
    case "$1" in
    --file)
        if ! [[ $# -ge 2 && -f "$2" && "$2" =~ ^engine/(src|include)/ ]]; then
            echo "--file needs an engine source or header, as engine/src/Analyzer.cpp." >&2
            exit 1
        fi
        files+=("$2")
        shift 2
        ;;
    *)
        base="$1"
        shift
        ;;
    esac
done

if ! command -v mull-runner-18 > /dev/null; then
    command -v docker > /dev/null ||
        { echo "Needs mull-runner-18 (Mull for Clang 18) or Docker." >&2; exit 1; }
    docker build -q -t sineless-mutation-cpp tools/mutation > /dev/null
    # The build stays in a volume of its own: Linux objects have no place in the host's build/.
    # MUTANT_TIMEOUT goes in with the run, when set.
    exec docker run --rm -e MUTANT_TIMEOUT -v "$root":/src \
        -v sineless-mutation-cpp:/src/build/mutation-cpp -w /src \
        sineless-mutation-cpp scripts/mutation-cpp.sh ${args[@]+"${args[@]}"}
fi

rm -rf "$out"
mkdir -p "$out"
config="$root/tools/mutation/mull.yml"
if [[ -n "$base" ]]; then
    from="$(git merge-base "$base" HEAD)"
    if git diff --quiet "$from" -- engine/src engine/include; then
        printf '## Mutation testing (C++)\n\nNo engine source lines changed since %s.\n' "$base" \
            > "$out/summary.md"
        cat "$out/summary.md"
        exit 0
    fi
    # The tests run only the mutants on the lines changed since the base; the build compiles them
    # all, so a cached build serves any base.
    config="$out/mull.yml"
    { cat tools/mutation/mull.yml; echo "gitDiffRef: $from"; echo "gitProjectRoot: $root"; } \
        > "$config"
fi
if ((${#files[@]})); then
    # The tests run only the mutants in the files named, from the same build: the run's own
    # settings name just those files.
    filtered="$out/mull-files.yml"
    {
        sed '/^includePaths:/,/^[a-zA-Z]/{/^includePaths:/d;/^  - /d;}' "$config"
        echo "includePaths:"
        for file in "${files[@]}"; do
            # Single quotes: YAML reads backslashes in them as they are, regex escapes included.
            printf "  - '.*/%s\$'\n" "${file//./\\.}"
        done
    } > "$filtered"
    config="$filtered"
fi

# The mutants compiled in follow mull.yml and the Mull installed, which neither Ninja nor ccache
# sees: the build starts afresh when either changes, and ccache hashes both.
settings=(tools/mutation/mull.yml tools/mutation/install-mull.sh)
stamp="$(cat "${settings[@]}" | sha256sum | cut -d' ' -f1)"
if [[ "$(cat build/mutation-cpp/settings.sha256 2> /dev/null)" != "$stamp" ]]; then
    rm -rf build/mutation-cpp/engine
fi
export CCACHE_EXTRAFILES="$root/tools/mutation/mull.yml:$root/tools/mutation/install-mull.sh"
MULL_CONFIG="$root/tools/mutation/mull.yml" cmake --preset mutation > "$out/configure.log"
MULL_CONFIG="$root/tools/mutation/mull.yml" cmake --build --preset mutation
echo "$stamp" > build/mutation-cpp/settings.sha256
start=$SECONDS
# A full run of the tests takes about 5 minutes in this build in Docker on macOS (Mull's
# instrumentation) and less on CI; a mutant may hang one, which then ends after MUTANT_TIMEOUT
# seconds (default 900; CI sets 300, its runs being faster). A run against a base, as on CI, stops
# after 75 minutes, so that CI's 90-minute limit never cancels the job; a run over the whole engine
# is not limited.
limit=()
if [[ -n "$base" ]]; then
    limit=(timeout 75m)
fi
status=0
mutantTimeout=$((${MUTANT_TIMEOUT:-900} * 1000))
MULL_CONFIG="$config" "${limit[@]}" mull-runner-18 --workers "$(nproc)" --timeout "$mutantTimeout" \
    --reporters Elements --report-dir "$out" --report-name mull \
    build/mutation-cpp/engine/engine_tests --gtest_fail_fast > "$out/mull.log" 2>&1 || status=$?
if [[ -n "$base" ]] && ((status == 124)); then
    printf '## Mutation testing (C++)\n\nStopped after 75 minutes: too many mutants on the lines changed since %s.\n' \
        "$base" > "$out/summary.md"
    cat "$out/summary.md"
    exit 0
fi
if [[ ! -f "$out/mull.json" ]]; then
    tail -n 20 "$out/mull.log" >&2
    exit 1
fi
node scripts/mutation-report.ts "$out/mull.json" "Mutation testing (C++)" \
    "$((SECONDS - start))" > "$out/summary.md"
# A mutant no test notices runs every test: were that close to its limit, it would be stopped and
# reported as timed out, which counts as noticed, hiding a line no test checks. The summary says
# how long the unmutated tests took (Mull's warm-up run) against the limit, and warns past half.
warmup="$(grep -A1 'Warm up run' "$out/mull.log" | grep -oE 'Finished in [0-9hms.]+' | head -1 |
    sed 's/Finished in //' |
    awk '{ s = 0; n = ""; for (i = 1; i <= length($0); i++) { c = substr($0, i, 1);
        if (c ~ /[0-9.]/) n = n c; else { s += n * (c == "h" ? 3600 : c == "m" ? 60 : 1); n = "" } }
        printf "%d", s }')"
if [[ -n "$warmup" ]]; then
    limitSeconds=$((mutantTimeout / 1000))
    printf '\nThe tests take %d s unmutated; each mutant is stopped at %d s.\n' "$warmup" \
        "$limitSeconds" >> "$out/summary.md"
    if ((warmup * 2 > limitSeconds)); then
        {
            printf '\n> [!WARNING]\n> The tests take more than half the limit: raise'
            printf ' MUTANT_TIMEOUT, or mutants no test notices may be reported as timed out.\n'
        } >> "$out/summary.md"
    fi
fi
cat "$out/summary.md"
