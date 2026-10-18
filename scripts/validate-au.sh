#!/usr/bin/env bash
# Validates the Audio Unit as hosts meet it: installs the component for this user, waits until
# macOS has registered it, then runs Apple's auval on it (strict, and once out of process,
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
replacing=false
[[ -e "$installed" ]] && replacing=true
rm -rf "$installed"
ditto "$component" "$installed"
codesign --force --sign - "$installed"

# Registration is not immediate. A copy that replaces an earlier one may keep the earlier one's
# registration (same version, other metadata), so the registrar is restarted at once to rescan;
# a first install is left to the registrar, which normally notices it within a few seconds, and
# restarted only if it has not after 10 s. A restart rescans every component, so there is at most
# one. Then the script waits until auval finds the component at this build's version: 30 tries 2 s
# apart, 60 s in all, far beyond the few seconds registration takes, for slow CI runners.
restartRegistrar() {
    killall -9 AudioComponentRegistrar 2> /dev/null || true
    restarted=true
}
registered() {
    auval -v "${codes[@]}" > "$log" 2>&1 || true
    grep -q "Component Version: .*($versionHex)" "$log"
}
restarted=false
[[ "$replacing" == true ]] && restartRegistrar
for attempt in $(seq 1 30); do
    registered && break
    if [[ "$restarted" == false && "$attempt" -gt 5 ]]; then
        restartRegistrar
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
