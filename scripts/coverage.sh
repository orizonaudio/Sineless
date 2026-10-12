#!/usr/bin/env bash
# Measures which lines the tests run (Linux, Clang): the C++ engine with source-based coverage
# over the engine and protocol tests, and the TypeScript with V8's over the unit and app tests
# (the app tests in Chromium, where V8 measures). Prints a summary of each and writes lcov files
# and a Markdown summary to build/coverage/ (cpp.lcov, ts/lcov.info, summary.md). With a base
# revision, also reports the coverage of the lines changed since it:
#   cmake --preset coverage && cmake --build --preset coverage
#   scripts/coverage.sh [base revision, e.g. origin/main]
# A report, not a check: tests that fail under coverage are noted in the summary, and the script
# then exits 1 after writing it.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build/coverage"
base="${1:-}"
cd "$root"
failures=()

# The LLVM tools of the Clang that built the tests.
version="$(clang --version | sed -nE 's/.*clang version ([0-9]+).*/\1/p' | head -n 1)"
tool() {
    command -v "$1-$version" || command -v "$1" ||
        { echo "Missing $1-$version and $1 (the LLVM tools of Clang $version)." >&2; exit 1; }
}
profdata="$(tool llvm-profdata)"
cov="$(tool llvm-cov)"

# Profiles from an earlier build would mix with this one's: each run starts afresh.
rm -rf "$build/profiles" "$build/ts" "$build/summary.md" "$build/changed-lines.md"
ctest --preset coverage > "$build/ctest.log" 2>&1 || failures+=("C++ tests (see $build/ctest.log)")

shopt -s nullglob
profiles=("$build"/profiles/*.profraw)
shopt -u nullglob
if ((${#profiles[@]} == 0)); then
    echo "No profiles in $build/profiles: build the coverage preset first." >&2
    exit 1
fi
"$profdata" merge -sparse "${profiles[@]}" -o "$build/tests.profdata"

# Every test executable, as the first object and the rest with -object.
mapfile -t binaries < <(find "$build" -type f -perm -u+x -name '*_tests' | sort)
objects=("${binaries[0]}")
for binary in "${binaries[@]:1}"; do
    objects+=(-object "$binary")
done
# The engine's own sources only, named by path: not the tests, the generated protocol header or
# the dependencies, wherever those were fetched from.
sources=("$root/engine/src" "$root/engine/include")

cppSummary="$("$cov" report "${objects[@]}" -instr-profile="$build/tests.profdata" "${sources[@]}" |
    tail -n 1 |
    awk '{ printf "regions %s, functions %s, lines %s, branches %s", $4, $7, $10, $13 }')"
"$cov" export "${objects[@]}" -instr-profile="$build/tests.profdata" -format=lcov \
    "${sources[@]}" > "$build/cpp.lcov"
echo "== C++ (engine): $cppSummary"

# The coverage report is written even when a test fails, so that the summary still shows it.
vitestStatus=0
APP_BROWSERS=chromium npx vitest run --coverage --coverage.reportOnFailure \
    --coverage.reporter=text-summary --coverage.reporter=lcov \
    --coverage.reportsDirectory="$build/ts" > "$build/vitest.log" 2>&1 || vitestStatus=$?
if ((vitestStatus != 0)); then
    failures+=("TypeScript tests (see $build/vitest.log)")
fi
tsSummary="$(sed -nE 's/^(Statements|Branches|Functions|Lines) *: ([0-9.]+%).*/\1 \2/p' \
    "$build/vitest.log" | tr '[:upper:]' '[:lower:]' | paste -sd ',' | sed 's/,/, /g')"
echo "== TypeScript (protocol, ui): ${tsSummary:-no report}"

{
    echo "## Coverage"
    echo
    echo "- C++ (engine): $cppSummary"
    echo "- TypeScript (protocol, ui; app tests in Chromium): ${tsSummary:-no report}"
    for failure in "${failures[@]}"; do
        echo "- Failed under coverage: $failure"
    done
} > "$build/summary.md"

if [[ -n "$base" && -f "$build/ts/lcov.info" ]]; then
    echo "== Lines changed since $base"
    # diff-cover reads lcov paths relative to the repository.
    sed -i "s|^SF:$root/|SF:|" "$build/cpp.lcov" "$build/ts/lcov.info"
    if command -v pipx > /dev/null; then
        diffCover=(pipx run --spec diff-cover==10.6.0 diff-cover)
    else
        diffCover=(uvx --from diff-cover==10.6.0 diff-cover)
    fi
    "${diffCover[@]}" "$build/cpp.lcov" "$build/ts/lcov.info" \
        --compare-branch="$base" --format "markdown:$build/changed-lines.md"
    { echo; cat "$build/changed-lines.md"; } >> "$build/summary.md"
fi

if ((${#failures[@]} > 0)); then
    printf 'Failed under coverage: %s\n' "${failures[@]}" >&2
    exit 1
fi
