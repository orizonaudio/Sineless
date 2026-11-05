#!/usr/bin/env bash
# Reports a failed nightly fuzz run as an issue, so that it is seen whoever's notifications are on
# (GitHub tells only the user who last edited a scheduled workflow's cron line). One issue at a
# time: while one labelled fuzz-failure is open, later failures comment on it. Run by
# fuzz-nightly.yml after scripts/fuzz.sh, from what it left in build/fuzz/artifacts and
# build/fuzz/logs.
# Usage: RUN_URL=<the run's page> scripts/fuzz-failure-issue.sh [--dry-run]
# With --dry-run it prints the gh commands and the text instead of running them, and looks up no
# issue (DRY_RUN_OPEN_ISSUE=<number> pretends one is open). gh reads GH_TOKEN and GH_REPO.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="${FUZZ_BUILD:-$root/build/fuzz}"
dryRun=false
if [ "${1:-}" = --dry-run ]; then
    dryRun=true
fi
runUrl="${RUN_URL:?RUN_URL must name the failed run}"
label=fuzz-failure
logLines=60

gh_() {
    if $dryRun; then
        printf 'gh'
        printf ' %q' "$@"
        printf '\n'
    else
        gh "$@"
    fi
}

# The targets that failed, from the inputs libFuzzer saved as <fuzzer>-<kind>-<hash>.
failed=()
shopt -s nullglob
for artifact in "$build/artifacts"/*; do
    name="$(basename "$artifact")"
    fuzzer="$(sed -E 's/-(crash|timeout|oom|leak|slow-unit)-.*$//' <<<"$name")"
    [ "$fuzzer" != "$name" ] || continue
    case " ${failed[*]-} " in
    *" $fuzzer "*) ;;
    *) failed+=("$fuzzer") ;;
    esac
done
shopt -u nullglob

body="$(mktemp)"
trap 'rm -f "$body"' EXIT
{
    echo "Nightly fuzzing failed: $runUrl"
    echo
    if [ ${#failed[@]} -eq 0 ]; then
        echo "No fuzz target saved an input, so the run failed outside fuzzing: the build, a" \
            "replayed regression, the pruning or a timeout. The run's log says which."
    else
        echo "Failed: ${failed[*]}."
        for fuzzer in "${failed[@]}"; do
            echo
            echo "### $fuzzer"
            echo
            log="$build/logs/$fuzzer.log"
            if [ -f "$log" ]; then
                echo "The last $logLines lines of its log:"
                echo
                echo '```'
                tail -n "$logLines" "$log"
                echo '```'
            else
                echo "Its log is missing."
            fi
        done
        echo
        echo "To reproduce, download the run's fuzz-crashes artifact, build the fuzz preset and run" \
            "the target on the input:"
        echo
        echo '```'
        echo 'cmake --preset fuzz && cmake --build --preset fuzz'
        echo 'build/fuzz/engine/tests/fuzz/<fuzzer> <the saved input>'
        echo '```'
        echo
        echo "Once fixed, the input goes in engine/tests/fuzz/regressions/<fuzzer>/ with the fix."
    fi
} >"$body"

if $dryRun; then
    cat "$body"
    echo
    open="${DRY_RUN_OPEN_ISSUE-}" # The open issue a dry run pretends exists, if any.
else
    open="$(gh issue list --label "$label" --state open --limit 1 --json number --jq '.[0].number // empty')"
fi

if [ -n "$open" ]; then
    gh_ issue comment "$open" --body-file "$body"
else
    gh_ label create "$label" --color B60205 --description "A nightly fuzz run failed" --force
    gh_ issue create --title "Nightly fuzzing failed" --label "$label" --body-file "$body"
fi
