#!/usr/bin/env bash
# Validates the Audio Unit as hosts meet it: installs the component for this user, waits until
# macOS has registered this build, then runs Apple's auval on it (strict, and once out of process,
# as Logic and GarageBand load it) and pluginval. The installed copy stays: the next build or the
# installer replaces it.
#
# Usage: scripts/validate-au.sh <Sineless.component> <pluginval executable>
set -euo pipefail

component="$1"
pluginval="$2"
codes=(aufx Snls Oriz)
installed="$HOME/Library/Audio/Plug-Ins/Components/Sineless.component"
log="$(mktemp)"
trap 'rm -f "$log"' EXIT

# The version hosts compare, as auval prints it ("Component Version: 0.5.2 (0x502)").
version="$(/usr/libexec/PlistBuddy -c 'Print :AudioComponents:0:version' \
    "$component/Contents/Info.plist")"
versionHex="$(printf '0x%X' "$version")"

mkdir -p "$(dirname "$installed")"
rm -rf "$installed"
ditto "$component" "$installed"
codesign --force --sign - "$installed"

# Registration is not immediate, and macOS may still serve the previous build's: wait until auval
# finds the component at this build's version. Restarting the registrar makes it rescan every
# component, so it is done at most once.
registered() {
    auval -v "${codes[@]}" > "$log" 2>&1 || true
    grep -q "Component Version: .*($versionHex)" "$log"
}
restarted=false
for attempt in $(seq 1 30); do
    registered && break
    if [[ "$restarted" == false && "$attempt" -ge 5 ]]; then
        killall -9 AudioComponentRegistrar 2> /dev/null || true
        restarted=true
    fi
    sleep 2
done
if ! registered; then
    cat "$log"
    echo "macOS did not register Sineless.component at version $versionHex" >&2
    exit 1
fi

auval -strict -v "${codes[@]}"
auval -strict -v "${codes[@]}" -oop
"$pluginval" --strictness-level 10 --validate-in-process --validate "$installed"
