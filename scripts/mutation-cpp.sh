#!/usr/bin/env bash
# Mutation testing of the C++ engine, plugin and editor: Mull builds their tests with every mutant
# compiled in (tools/mutation/mull.yml) and runs each mutant against its own module's tests, each
# run stopping at the first test that fails. A mutant no test fails on marks code that no test
# checks. With a base revision, only the lines changed since it are mutated, as on a pull request
# (and nothing is built when none changed); with --file, only the files named (a source or header
# each of the engine, plugin or editor, repeatable); with neither, all of them, about nine hours
# for the engine:
#   scripts/mutation-cpp.sh [--file engine/src/Analyzer.cpp ...] [base revision, e.g. origin/main]
# Mull lists each module's mutants from its own tests (its dry run, which also times them with
# none), and scripts/mutation-run.ts runs them: each stopped after its module's limit (below), and
# none started after MUTATION_DEADLINE seconds of running (default: none), those left being
# reported as not run rather than lost. CI shares the mutants out:
#   scripts/mutation-cpp.sh --list [--file ...] [base]   builds and lists them only
#   scripts/mutation-cpp.sh --share <i>/<n>              runs share i of n, from that build and list
#   scripts/mutation-cpp.sh --merge                      reports the shares' results together
# Mull needs the Clang it was built for: Ubuntu 24.04's Clang 18, as on CI. Where mull-runner-18 is
# not installed (on macOS), the script runs itself in tools/mutation/Dockerfile's image, with the
# packages the editor's page is built with installed there for Linux.
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
        sources='^(engine/(src|include)|plugin/src|editor/(src|include))/'
        if ! [[ $# -ge 2 && -f "$2" && "$2" =~ $sources ]]; then
            echo "--file needs a source or header of the engine, plugin or editor," \
                "as engine/src/Analyzer.cpp." >&2
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

# The modules mutated, each with its sources and its tests, which its mutants run against.
modules=(engine plugin editor)
sourcesOf() {
    case "$1" in
    engine) echo engine/src engine/include ;;
    plugin) echo plugin/src ;;
    editor) echo editor/src editor/include ;;
    esac
}
# With the VST3 SDK in the build, every executable is written to its bin/ folder.
testsOf() {
    echo "build/mutation-cpp/bin/RelWithDebInfo/$1_tests"
}
# Each mutant's time limit in its module, in seconds. The engine's tests take minutes with every
# mutant compiled in, from which CI sets MUTANT_TIMEOUT; the plugin's and editor's take a tenth of
# a second, so that a mutant hanging one (a read loop that never ends, say) is stopped within a
# minute rather than holding a worker for the engine's limit.
mutantTimeout="${MUTANT_TIMEOUT:-900}"
limitOf() {
    if [[ "$1" == engine ]]; then echo "$mutantTimeout"; else echo 60; fi
}
# Reports the results of the shares run so far against Mull's list: those of mutants no share ran
# are reported as not run. Takes no Mull, so CI's merge job needs only Node.
report() {
    node scripts/mutation-run.ts merge "$out/dry.json" "$out"/results-*.jsonl > "$out/mull.json"
    # With the Mull log and limit of each module with mutants, the summary also says how long its
    # tests take with no mutant against that limit, and warns when the margin is too small
    # (scripts/mutation-summary.ts).
    local times=() module
    for module in "${modules[@]}"; do
        if [[ -f "$out/dry-$module.json" ]]; then
            times+=(--tests-log "${module}_tests=$out/mull-$module.log"
                --tests-limit "${module}_tests=$(limitOf "$module")")
        fi
    done
    node scripts/mutation-report.ts "$out/mull.json" "Mutation testing (C++)" ${1:+"$1"} \
        ${times[@]+"${times[@]}"} > "$out/summary.md"
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
    # The build and the page's packages each stay in a volume of their own: Linux objects and
    # packages have no place in the host's build/ and node_modules/. MUTANT_TIMEOUT and
    # MUTATION_DEADLINE go in with the run, when set.
    exec docker run --rm -e MUTANT_TIMEOUT -e MUTATION_DEADLINE -v "$root":/src \
        -v sineless-mutation-cpp:/src/build/mutation-cpp \
        -v sineless-mutation-cpp-node:/src/node_modules -w /src sineless-mutation-cpp \
        bash -c 'npm ci --no-audit --no-fund --no-update-notifier --loglevel=error > /dev/null &&
            exec scripts/mutation-cpp.sh "$@"' mutation-cpp ${args[@]+"${args[@]}"}
fi

# Runs share $1 of the mutants Mull listed, from the build, recording each result as it is known:
# share $1 of each module's mutants against its own tests, stopped at its own limit. The plugin's
# and editor's take seconds, so they run first, and the engine's get what is left of the deadline;
# a module none is left for has its mutants reported as not run.
runShare() {
    local deadline="${MUTATION_DEADLINE:-1000000000}" start=$SECONDS module left
    for module in plugin editor engine; do
        left=$((deadline - (SECONDS - start)))
        if [[ -f "$out/dry-$module.json" ]] && ((left > 0)); then
            node scripts/mutation-run.ts run "$out/dry-$module.json" "$1" \
                "$out/results-${1%%/*}-$module.jsonl" --limit "$(limitOf "$module")" \
                --deadline "$left" -- "$(testsOf "$module")" --gtest_fail_fast
        fi
    done
}
if [[ "$mode" == share ]]; then
    for module in "${modules[@]}"; do
        if [[ ! -f "$out/dry.json" || ! -x "$(testsOf "$module")" ]]; then
            echo "--share needs the build and the list of a --list run in build/." >&2
            exit 1
        fi
    done
    runShare "$share"
    exit 0
fi

rm -rf "$out"
mkdir -p "$out"
config="$root/tools/mutation/mull.yml"
# The modules to list mutants in: those of the files named, or all.
selected=()
for module in "${modules[@]}"; do
    named=0
    for file in ${files[@]+"${files[@]}"}; do
        if [[ "${file%%/*}" == "$module" ]]; then named=1; fi
    done
    if ((named || ${#files[@]} == 0)); then
        selected+=("$module")
    fi
done
if [[ -n "$base" ]]; then
    from="$(git merge-base "$base" HEAD)"
    # Only the modules whose sources changed: Mull's dry run times a module's tests before it
    # filters the mutants, minutes for the engine's even with none on the lines changed.
    changed=()
    for module in "${selected[@]}"; do
        # Unquoted: each of the module's source folders is a path of its own.
        git diff --quiet "$from" -- $(sourcesOf "$module") || changed+=("$module")
    done
    if ((${#changed[@]} == 0)); then
        printf '## Mutation testing (C++)\n\nNo C++ source lines changed since %s.\n' "$base" \
            > "$out/summary.md"
        cat "$out/summary.md"
        exit 0
    fi
    selected=("${changed[@]}")
    # The tests run only the mutants on the lines changed since the base; the build compiles them
    # all, so a cached build serves any base.
    config="$out/mull.yml"
    # Mull is given the base with the files added since it held empty, as its diff leaves out
    # files the base lacks (scripts/mutation-base.ts).
    # Unquoted: each source folder of the modules changed is a path of its own.
    diffBase="$(node scripts/mutation-base.ts "$from" $(for module in "${selected[@]}"; do
        sourcesOf "$module"
    done))"
    { cat tools/mutation/mull.yml; echo "gitDiffRef: $diffBase"; echo "gitProjectRoot: $root"; } \
        > "$config"
fi

# The mutants compiled in follow mull.yml and the Mull installed, which neither Ninja nor ccache
# sees: the build starts afresh when either changes, and ccache hashes both.
settings=(tools/mutation/mull.yml tools/mutation/install-mull.sh)
stamp="$(cat "${settings[@]}" | sha256sum | cut -d' ' -f1)"
if [[ "$(cat build/mutation-cpp/settings.sha256 2> /dev/null)" != "$stamp" ]]; then
    rm -rf build/mutation-cpp/engine build/mutation-cpp/plugin build/mutation-cpp/editor
fi
export CCACHE_EXTRAFILES="$root/tools/mutation/mull.yml:$root/tools/mutation/install-mull.sh"
MULL_CONFIG="$root/tools/mutation/mull.yml" cmake --preset mutation > "$out/configure.log"
MULL_CONFIG="$root/tools/mutation/mull.yml" cmake --build --preset mutation
echo "$stamp" > build/mutation-cpp/settings.sha256
start=$SECONDS
# Each module's mutants, listed with no test run from its own tests, as the run's own settings name
# its sources (or the files named in it). Mull's warm-up still runs those tests once with no
# mutant, which the summary reports against each mutant's limit.
lists=()
for module in "${selected[@]}"; do
    moduleConfig="$out/mull-$module.yml"
    {
        sed '/^includePaths:/,/^[a-zA-Z]/{/^includePaths:/d;/^  - /d;}' "$config"
        echo "includePaths:"
        if ((${#files[@]})); then
            for file in "${files[@]}"; do
                # Single quotes: YAML reads backslashes in them as they are, regex escapes included.
                if [[ "${file%%/*}" == "$module" ]]; then
                    printf "  - '.*/%s\$'\n" "${file//./\\.}"
                fi
            done
        else
            for folder in $(sourcesOf "$module"); do
                printf "  - '.*/%s/.*'\n" "$folder"
            done
        fi
    } > "$moduleConfig"
    MULL_CONFIG="$moduleConfig" mull-runner-18 --dry-run --timeout 3600000 --reporters Elements \
        --report-dir "$out" --report-name "dry-$module" "$(testsOf "$module")" --gtest_fail_fast \
        > "$out/mull-$module.log" 2>&1 || true
    # No list: nothing to judge in the module when no mutant is on its lines changed, as when only
    # comments changed, and a failed run otherwise (scripts/mutation-summary.ts, noMutantFound).
    if [[ -f "$out/dry-$module.json" ]]; then
        lists+=("$out/dry-$module.json")
    elif ! node scripts/mutation-report.ts "$out/dry-$module.json" "Mutation testing (C++)" \
        --mull-dry-log "$out/mull-$module.log" --base "${base:-HEAD}" > "$out/summary.md"; then
        rm -f "$out/summary.md"
        tail -n 20 "$out/mull-$module.log" >&2
        exit 1
    fi
done
if ((${#lists[@]} == 0)); then
    cat "$out/summary.md"
    exit 0
fi
rm -f "$out/summary.md"
node scripts/mutation-run.ts combine "${lists[@]}" > "$out/dry.json"
if [[ "$mode" == list ]]; then
    exit 0
fi
runShare 1/1
report "$((SECONDS - start))"
