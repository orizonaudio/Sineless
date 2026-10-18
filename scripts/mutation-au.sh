#!/usr/bin/env bash
# Mutation testing of the Audio Unit (macOS): Mull builds the Audio Unit's tests with every mutant
# in au/src compiled in (tools/mutation/mull-au.yml), and each mutant runs against au_tests, then,
# while none has noticed it, au_view_tests and au_component_tests (which loads the component, built
# with the same mutants); each stops at its first failing test. A mutant none notices marks code
# that no test checks. With a base revision, only the au/src lines changed since it are mutated,
# as on a pull request (and nothing is built when none changed); without, all of au/src, a few
# minutes:
#   scripts/mutation-au.sh [base revision, e.g. origin/main]
# Mull lists the mutants au_tests holds, all of au/src's but those of the component's one-call
# entry point, src/ComponentEntry.cpp, which only the component holds (its dry run), and
# scripts/mutation-run.ts runs them one at a time (the view's tests share the window server), each
# test binary stopped after MUTANT_TIMEOUT seconds (default 300), and none started after
# MUTATION_DEADLINE seconds of running them (default: none), those left being reported as not run.
# Mull for LLVM 20 (tools/mutation/install-mull-macos.sh, into build/mull-macos) needs Homebrew's
# llvm@20 and builds against a macOS 26 SDK: Clang 20 cannot build the tests against the macOS 27
# SDK, and Mull has no macOS build for a newer LLVM. Mull mutates C++ only: the view's Objective-C
# methods get no mutant, its C++ functions do.
# A report, not a check: writes build/mutation-au-report/ (mull.json and summary.md, which also
# says how long the run took) and exits 0 whatever the tests do not notice.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/build/mutation-au-report"
cd "$root"
base="${1:-}"
mutantTimeout="${MUTANT_TIMEOUT:-300}"

rm -rf "$out"
mkdir -p "$out"
config="$root/tools/mutation/mull-au.yml"
if [[ -n "$base" ]]; then
    from="$(git merge-base "$base" HEAD)"
    if git diff --quiet "$from" -- au/src; then
        printf '## Mutation testing (Audio Unit)\n\nNo Audio Unit source lines changed since %s.\n' \
            "$base" > "$out/summary.md"
        cat "$out/summary.md"
        exit 0
    fi
    # The tests run only the mutants on the lines changed since the base; the build compiles them
    # all, so a cached build serves any base.
    config="$out/mull.yml"
    # Mull is given the base with the files added since it held empty, as its diff leaves out
    # files the base lacks (scripts/mutation-base.ts).
    diffBase="$(node scripts/mutation-base.ts "$from" au/src)"
    { cat tools/mutation/mull-au.yml; echo "gitDiffRef: $diffBase"; echo "gitProjectRoot: $root"; } \
        > "$config"
fi

[[ -x /opt/homebrew/opt/llvm@20/bin/clang++ ]] ||
    { echo "Needs Homebrew's LLVM 20 (brew install llvm@20)." >&2; exit 1; }
# Xcode's SDK where it is a macOS 26 one (as on CI's macos-26 runners), else the newest macOS 26 SDK
# the Command Line Tools hold.
sdk="$(xcrun --show-sdk-path)"
if [[ "$(xcrun --show-sdk-version)" != 26.* ]]; then
    sdk="$(find /Library/Developer/CommandLineTools/SDKs -maxdepth 1 -name 'MacOSX26*.sdk' \
        2> /dev/null | sort -V | tail -n 1 || true)"
    [[ -n "$sdk" ]] || {
        echo "Needs a macOS 26 SDK: Clang 20 cannot build against Xcode's $(xcrun --show-sdk-version)." >&2
        exit 1
    }
fi

# Mull reads each file's recorded compile command again and splits it at spaces: built from a path
# with a space in it, the tests build with no mutant at all. Such a checkout is built through a link
# without one; the reports still name the files at their real paths.
src="$root"
if [[ "$root" == *" "* ]]; then
    src="${TMPDIR:-/tmp}/sineless-mutation-au-$(printf %s "$root" | shasum | cut -c1-12)"
    ln -sfn "$root" "$src"
fi

# The mutants compiled in follow mull-au.yml and the Mull installed, which neither Make nor ccache
# sees: the build starts afresh when either changes, and ccache hashes both.
settings=(tools/mutation/mull-au.yml tools/mutation/install-mull-macos.sh)
stamp="$(cat "${settings[@]}" | shasum -a 256 | cut -d' ' -f1)"
if [[ "$(cat build/mull-macos/settings.sha256 2> /dev/null)" != "$stamp" ]]; then
    tools/mutation/install-mull-macos.sh build/mull-macos
    rm -rf build/mutation-au
    echo "$stamp" > build/mull-macos/settings.sha256
fi
export CCACHE_EXTRAFILES="$root/tools/mutation/mull-au.yml:$root/tools/mutation/install-mull-macos.sh"
(
    cd "$src"
    MULL_CONFIG="$root/tools/mutation/mull-au.yml" cmake --preset mutation-au \
        -DCMAKE_OSX_SYSROOT="$sdk" > "$out/configure.log"
    MULL_CONFIG="$root/tools/mutation/mull-au.yml" cmake --build --preset mutation-au \
        -j "$(sysctl -n hw.ncpu)" 2>&1 | tee "$out/build.log"
)
if grep -q "Cannot parse file" "$out/build.log"; then
    echo "Mull could not read a file's compile command, so its mutants are missing; see $out/build.log." >&2
    exit 1
fi

bin="$root/build/mutation-au/bin/RelWithDebInfo"
binaries=("$bin/au_tests" "$bin/au_view_tests.app/Contents/MacOS/au_view_tests"
    "$bin/au_component_tests.app/Contents/MacOS/au_component_tests")
tests=("${binaries[0]}" --gtest_fail_fast -- "${binaries[1]}" --gtest_fail_fast
    -- "${binaries[2]}" --gtest_fail_fast)
start=$SECONDS
# The mutants, listed with no test run: Mull's warm-up still runs au_tests once with none, which
# the summary does not use: it weighs each binary's own time instead.
MULL_CONFIG="$config" "$root/build/mull-macos/bin/mull-runner-20" --dry-run --timeout 3600000 \
    --reporters Elements --report-dir "$out" --report-name dry "${tests[0]}" > "$out/mull.log" 2>&1 ||
    true
# No list: nothing to judge when no mutant is on the lines changed, as when only comments changed,
# and a failed run otherwise (scripts/mutation-summary.ts, noMutantFound).
if [[ ! -f "$out/dry.json" ]]; then
    if node scripts/mutation-report.ts "$out/dry.json" "Mutation testing (Audio Unit)" \
        --mull-dry-log "$out/mull.log" --base "${base:-HEAD}" > "$out/summary.md"; then
        cat "$out/summary.md"
        exit 0
    fi
    rm -f "$out/summary.md"
    tail -n 20 "$out/mull.log" >&2
    exit 1
fi
# Each test binary's time with no mutant, the slowest of which the summary weighs against the
# limit: a mutant none notices runs each in turn, and one stopped at the limit counts as noticed.
slowest=0
for binary in "${binaries[@]}"; do
    begin=$SECONDS
    "$binary" --gtest_brief=1 > /dev/null 2>&1 || true
    if ((SECONDS - begin > slowest)); then
        slowest=$((SECONDS - begin))
    fi
done
node scripts/mutation-run.ts run "$out/dry.json" 1/1 "$out/results-1.jsonl" \
    --limit "$mutantTimeout" --deadline "${MUTATION_DEADLINE:-1000000000}" --workers 1 -- "${tests[@]}"
node scripts/mutation-run.ts merge "$out/dry.json" "$out/results-1.jsonl" > "$out/mull.json"
node scripts/mutation-report.ts "$out/mull.json" "Mutation testing (Audio Unit)" \
    "$((SECONDS - start))" --binary-seconds "$slowest" --limit "$mutantTimeout" > "$out/summary.md"
cat "$out/summary.md"
