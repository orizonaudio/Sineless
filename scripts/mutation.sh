#!/usr/bin/env bash
# Mutation testing of the TypeScript: Stryker makes small changes to the product's code (mutants),
# one at a time, and runs the unit tests on each. A mutant no test fails on marks code that no test
# checks. With a base revision, only the lines changed since it are mutated, as on a pull request;
# without one, all of protocol/src and ui/src:
#   scripts/mutation.sh [base revision, e.g. origin/main]
# Stryker runs in a copy of the TypeScript (build/mutation/work), with the packages of
# tools/mutation: its own Vitest, as Stryker does not support the repository's Vitest 5 yet. Its
# lockfile is made with npm 11 (`npx npm@11 install` there): npm 10 fails on Vitest 4's peers.
# A report, not a check: writes build/mutation/ (mutation.json, mutation.html and summary.md, which
# also says how long Stryker took) and exits 0 whatever the tests do not notice.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/build/mutation"
base="${1:-}"
cd "$root"
work="$out/work"
# A run starts afresh, but keeps the packages installed for the last one.
rm -rf "$out"/mutation.json "$out"/mutation.html "$out"/summary.md "$work"/protocol "$work"/ui
mkdir -p "$work"

# The changed lines of each source file, as Stryker's file:first-last ranges. Lines only removed
# leave nothing to mutate.
mutate=()
if [[ -n "$base" ]]; then
    while IFS= read -r range; do
        mutate+=("$range")
    done < <(git diff --unified=0 --no-color "$(git merge-base "$base" HEAD)" -- \
        'protocol/src/*.ts' 'ui/src/*.ts' |
        awk '
            /^\+\+\+ / { file = ($2 == "/dev/null") ? "" : substr($2, 3); next }
            /^@@ / {
                split($3, added, ",")
                first = substr(added[1], 2)
                count = (2 in added) ? added[2] : 1
                if (file != "" && count > 0) {
                    print file ":" first "-" (first + count - 1)
                }
            }')
    if ((${#mutate[@]} == 0)); then
        printf '## Mutation testing (TypeScript)\n\nNo TypeScript source lines changed since %s.\n' \
            "$base" > "$out/summary.md"
        cat "$out/summary.md"
        exit 0
    fi
fi

args=()
if ((${#mutate[@]} > 0)); then
    args=(--mutate "$(IFS=,; echo "${mutate[*]}")")
fi
# The tracked files only, as they are in the working tree.
git ls-files -z -- protocol ui tsconfig.base.json vitest.mutation.config.ts vitest.unit.ts |
    tar --null -T - -cf - | tar -xf - -C "$work"
cp tools/mutation/package.json tools/mutation/stryker.config.json "$work/"
# Installed again only when the lockfile changed, and forgotten if the install fails.
if ! cmp -s tools/mutation/package-lock.json "$work/package-lock.json"; then
    cp tools/mutation/package-lock.json "$work/"
    (cd "$work" && npm ci --no-fund --no-audit) || { rm -f "$work/package-lock.json"; exit 1; }
fi
start=$SECONDS
# allowEmpty (tools/mutation/stryker.config.json): when no unit test reaches the lines changed,
# Stryker ends without a report rather than failing. Those lines are left to the app tests, which
# drive real browsers and so do not run per mutant; the summary says so, and the mutation triage
# finds nothing to judge.
(cd "$work" && npx stryker run "${args[@]+"${args[@]}"}") | tee "$out/stryker.log"
if [[ ! -f "$out/mutation.json" ]] && grep -q "No tests were found" "$out/stryker.log"; then
    printf '## Mutation testing (TypeScript)\n\nNo unit test reaches the TypeScript lines changed since %s: they are left to the app tests.\n' \
        "$base" > "$out/summary.md"
    cat "$out/summary.md"
    exit 0
fi
node scripts/mutation-report.ts "$out/mutation.json" "Mutation testing (TypeScript)" \
    "$((SECONDS - start))" > "$out/summary.md"
cat "$out/summary.md"
