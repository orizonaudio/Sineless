#!/usr/bin/env bash
# Mutation testing of the C++ engine: Mull builds engine_tests with every mutant compiled in
# (tools/mutation/mull.yml) and runs the tests once per mutant, each run stopping at the first test
# that fails. A mutant no test fails on marks code that no test checks. With a base revision, only
# the engine lines changed since it are mutated, as on a pull request (and nothing is built when
# none changed); with --file, only the files named (an engine source or header each, repeatable);
# with neither, all of engine/src and engine/include, about nine hours:
#   scripts/mutation-cpp.sh [--file engine/src/Analyzer.cpp ...] [base revision, e.g. origin/main]
# Mull lists the mutants (its dry run, which also times the tests with none), and
# scripts/mutation-run.ts runs them: each stopped after MUTANT_TIMEOUT seconds (default 900), and
# none started after MUTATION_DEADLINE seconds of running (default: none), those left being
# reported as not run rather than lost. CI shares the mutants out:
#   scripts/mutation-cpp.sh --list [--file ...] [base]   builds and lists them only
#   scripts/mutation-cpp.sh --share <i>/<n>              runs share i of n, from that build and list
#   scripts/mutation-cpp.sh --merge                      reports the shares' results together
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
mode=all
share=""
while (($#)); do
    case "$1" in
    --list)
        mode=list
        shift
        ;;
    --share)
        mode=share
        share="${2:-}"
        shift 2
        ;;
    --merge)
        mode=merge
        shift
        ;;
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

tests=(build/mutation-cpp/engine/engine_tests --gtest_fail_fast)
mutantTimeout="${MUTANT_TIMEOUT:-900}"
# Reports the results of the shares run so far against Mull's list: those of mutants no share ran
# are reported as not run. Takes no Mull, so CI's merge job needs only Node.
report() {
    node scripts/mutation-run.ts merge "$out/dry.json" "$out"/results-*.jsonl > "$out/mull.json"
    # With Mull's log and the limit, the summary also says how long the tests take with no mutant
    # against each mutant's limit, and warns when the margin is too small
    # (scripts/mutation-summary.ts).
    node scripts/mutation-report.ts "$out/mull.json" "Mutation testing (C++)" ${1:+"$1"} \
        --mull-log "$out/mull.log" --limit "$mutantTimeout" > "$out/summary.md"
    cat "$out/summary.md"
}
if [[ "$mode" == merge ]]; then
    if [[ ! -f "$out/dry.json" ]]; then
        echo "--merge needs the list of a --list run in $out." >&2
        exit 1
    fi
    report
    exit 0
fi

# A share needs no Mull either, only the Linux build it runs.
if [[ "$mode" != share || "$(uname)" != Linux ]] && ! command -v mull-runner-18 > /dev/null; then
    command -v docker > /dev/null ||
        { echo "Needs mull-runner-18 (Mull for Clang 18) or Docker." >&2; exit 1; }
    docker build -q -t sineless-mutation-cpp tools/mutation > /dev/null
    # The build stays in a volume of its own: Linux objects have no place in the host's build/.
    # MUTANT_TIMEOUT and MUTATION_DEADLINE go in with the run, when set.
    exec docker run --rm -e MUTANT_TIMEOUT -e MUTATION_DEADLINE -v "$root":/src \
        -v sineless-mutation-cpp:/src/build/mutation-cpp -w /src \
        sineless-mutation-cpp scripts/mutation-cpp.sh ${args[@]+"${args[@]}"}
fi

# Runs share $1 of the mutants Mull listed, from the build, recording each result as it is known.
runShare() {
    local deadline="${MUTATION_DEADLINE:-1000000000}"
    node scripts/mutation-run.ts run "$out/dry.json" "$1" "$out/results-${1%%/*}.jsonl" \
        --limit "$mutantTimeout" --deadline "$deadline" -- "${tests[@]}"
}
if [[ "$mode" == share ]]; then
    if [[ ! -f "$out/dry.json" || ! -x "${tests[0]}" ]]; then
        echo "--share needs the build and the list of a --list run in build/." >&2
        exit 1
    fi
    runShare "$share"
    exit 0
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
# The mutants, listed with no test run: Mull's warm-up still runs the tests once with none, which
# the summary reports against each mutant's limit.
MULL_CONFIG="$config" mull-runner-18 --dry-run --timeout 3600000 --reporters Elements \
    --report-dir "$out" --report-name dry "${tests[@]}" > "$out/mull.log" 2>&1 || true
if [[ ! -f "$out/dry.json" ]]; then
    tail -n 20 "$out/mull.log" >&2
    exit 1
fi
if [[ "$mode" == list ]]; then
    exit 0
fi
runShare 1/1
report "$((SECONDS - start))"
