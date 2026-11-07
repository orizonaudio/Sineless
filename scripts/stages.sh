# The check scripts' stages (sourced by scripts/check.sh and scripts/check-linux.sh): every stage
# runs, and the failures are listed at the end, so one run shows every finding. A stage that needs
# another runs only if that one passed. Bash 3.2, as macOS has it: no associative arrays, and an
# empty array is expanded as ${a[@]+"${a[@]}"} under set -u.
#
#   stage <name> [--needs <name>]... [--build] [--rerun <command>] -- <command> [args...]
#   stages_report     the summary; exits 1 if any stage failed
#
# BUILD_ONLY=1 runs only the stages marked --build, the builds that fill the compiler caches.

stageNames=()
stageResults=() # passed, failed, skipped or "not run"
stageNotes=()   # why: the exit status, the stage needed, or builds only
stageSeconds=()
stageReruns=()

stages_index() {
    local i
    for ((i = 0; i < ${#stageNames[@]}; i++)); do
        if [[ "${stageNames[i]}" == "$1" ]]; then
            echo "$i"
            return
        fi
    done
    echo -1
}

stages_record() {
    stageNames+=("$1")
    stageResults+=("$2")
    stageNotes+=("$3")
    stageSeconds+=("$4")
    stageReruns+=("$5")
}

stage() {
    local name="$1" build=false rerun="" needs=() need index
    shift
    while [[ $# -gt 0 && "$1" != -- ]]; do
        case "$1" in
        --needs) needs+=("$2") && shift 2 ;;
        --build) build=true && shift ;;
        --rerun) rerun="$2" && shift 2 ;;
        *) echo "stage $name: unknown option $1" >&2 && exit 2 ;;
        esac
    done
    shift
    printf '\n==> %s\n' "$name"
    if [[ "${BUILD_ONLY:-}" == 1 ]] && ! $build; then
        echo "Not run: builds only (BUILD_ONLY=1)."
        stages_record "$name" "not run" "builds only" "" ""
        return
    fi
    # A typo in a stage's needs would otherwise skip it silently.
    for need in ${needs[@]+"${needs[@]}"}; do
        index="$(stages_index "$need")"
        if ((index < 0)); then
            echo "stage $name needs \"$need\", which is not a stage before it." >&2
            exit 2
        fi
        if [[ "${stageResults[index]}" != passed ]]; then
            echo "Skipped: \"$need\" did not pass."
            stages_record "$name" skipped "needs $need" "" ""
            return
        fi
    done
    # Not in an if, && or ||, where -e would be ignored inside the stage too (Bash Reference
    # Manual, The Set Builtin): the subshell stops at its own first failing command.
    local start=$SECONDS status
    set +e
    (
        set -euo pipefail
        "$@"
    )
    status=$?
    set -e
    if ((status == 0)); then
        stages_record "$name" passed "" $((SECONDS - start)) ""
    else
        stages_record "$name" failed "exit $status" $((SECONDS - start)) "$rerun"
    fi
}

# Prints the summary; returns how many stages failed (0 when none).
stages_summary() {
    local i minutes failed=0 count=${#stageNames[@]} header=false
    printf '\n==> Summary\n'
    for ((i = 0; i < count; i++)); do
        minutes=""
        if [[ -n "${stageSeconds[i]}" ]]; then
            minutes="$(printf '%d.%d min' $((stageSeconds[i] / 60)) $((stageSeconds[i] % 60 / 6)))"
        fi
        if [[ "${stageResults[i]}" == failed ]]; then
            failed=$((failed + 1))
            printf '  %-8s %9s  %s (%s)\n' FAILED "$minutes" "${stageNames[i]}" "${stageNotes[i]}"
        elif [[ -n "${stageNotes[i]}" ]]; then
            printf '  %-8s %9s  %s (%s)\n' "${stageResults[i]}" "" "${stageNames[i]}" "${stageNotes[i]}"
        else
            printf '  %-8s %9s  %s\n' "${stageResults[i]}" "$minutes" "${stageNames[i]}"
        fi
    done
    for ((i = 0; i < count; i++)); do
        if [[ "${stageResults[i]}" == failed && -n "${stageReruns[i]}" ]]; then
            $header || echo "Rerun a failed stage alone:"
            header=true
            echo "  ${stageNames[i]}: ${stageReruns[i]}"
        fi
    done
    if ((failed > 0)); then
        printf '\n%d of %d stages failed.\n' "$failed" "$count"
    fi
    return "$failed"
}

stages_report() {
    if stages_summary; then
        printf '\nAll checks passed.\n'
    else
        exit 1
    fi
}

# Ctrl-C stops the whole run, not only the stage it interrupts.
stages_interrupted() {
    trap - INT TERM
    printf '\nInterrupted.\n'
    stages_summary || true
    exit 130
}
trap stages_interrupted INT TERM
