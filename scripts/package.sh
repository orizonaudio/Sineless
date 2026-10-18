#!/usr/bin/env bash
# Builds the release bundles (the VST3 plugin and the Audio Unit), zips them and wraps them in an
# installer for GitHub Releases. Neither has a Developer ID signature; the bundles are signed ad
# hoc, which seals each whole bundle (the linker only signs the binary).
#
# The installer puts each bundle in the user's own ~/Library/Audio/Plug-Ins (VST3 and Components)
# without asking for a password, and what Installer writes carries no quarantine flag, so unlike
# the zip it needs no Terminal step. Each format is a choice, selected by default, that Customize
# can leave out. With INSTALL_CHECK=1 (the release workflow, on a runner thrown away afterwards)
# it is also installed and the installed bundles checked; `installer` ignores HOME, so that
# installs into the real home folder.
#
# Output: build/release/Sineless-<version>-macOS.zip and build/release/Sineless-<version>-macOS.pkg
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

cmake --preset release
cmake --build --preset release -j "$(sysctl -n hw.ncpu)"

# The formats: bundle name, built bundle, installer package identifier, Plug-Ins folder, title.
names=(Sineless.vst3 Sineless.component)
built=(build/release/VST3/Release/Sineless.vst3 build/release/lib/Release/Sineless.component)
identifiers=(com.orizonaudio.sineless.vst3 com.orizonaudio.sineless.component)
folders=(VST3 Components)
titles=("VST3 plugin" "Audio Unit")

version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
    "${built[0]}/Contents/Info.plist")"
for bundle in "${built[@]}"; do
    bundleVersion="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
        "$bundle/Contents/Info.plist")"
    [[ "$bundleVersion" == "$version" ]] ||
        { echo "$bundle is version $bundleVersion, expected $version" >&2; exit 1; }
    # Checks of C-2: both architectures, macOS 13.1 minimum.
    binary="$bundle/Contents/MacOS/Sineless"
    for arch in arm64 x86_64; do
        lipo "$binary" -verify_arch "$arch" || { echo "$binary: no $arch slice" >&2; exit 1; }
    done
    minimum="$(vtool -show-build "$binary" | awk '/minos/ { print $2; exit }')"
    [[ "$minimum" == "13.1" ]] ||
        { echo "$binary: minimum macOS is $minimum, expected 13.1" >&2; exit 1; }
    codesign --force --sign - "$bundle"
    codesign --verify --strict "$bundle"
done

work="build/release/pkg"
rm -rf "$work"
mkdir -p "$work/zip" "$work/resources"

# The zip: both bundles at its top.
for i in "${!names[@]}"; do
    ditto "${built[$i]}" "$work/zip/${names[$i]}"
done
archive="build/release/Sineless-$version-macOS.zip"
rm -f "$archive"
ditto -c -k --norsrc --noextattr "$work/zip" "$archive"
top="$(zipinfo -1 "$archive" | awk -F/ '{ print $1 }' | sort -u | tr '\n' ' ')"
[[ "$top" == "Sineless.component Sineless.vst3 " ]] ||
    { echo "The zip holds $top at its top, expected both bundles alone" >&2; exit 1; }
echo "Packaged $archive ($(du -h "$archive" | cut -f1))"

# The installer: one component package per format, each built from a folder holding only its
# bundle. Not relocatable: Installer would otherwise "upgrade" a bundle it finds elsewhere, such
# as a build folder, instead of installing into the Plug-Ins folder. Upgrade replaces the whole
# bundle.
choices=""
outline=""
for i in "${!names[@]}"; do
    name="${names[$i]}"
    identifier="${identifiers[$i]}"
    mkdir -p "$work/root-$i"
    ditto "${built[$i]}" "$work/root-$i/$name"
    cat > "$work/component-$i.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<array>
    <dict>
        <key>RootRelativeBundlePath</key><string>$name</string>
        <key>BundleIsRelocatable</key><false/>
        <key>BundleIsVersionChecked</key><false/>
        <key>BundleOverwriteAction</key><string>upgrade</string>
    </dict>
</array>
</plist>
PLIST
    pkgbuild --root "$work/root-$i" --component-plist "$work/component-$i.plist" \
        --identifier "$identifier" --version "$version" \
        --install-location "/Library/Audio/Plug-Ins/${folders[$i]}" "$work/component-$i.pkg"
    # pkgbuild marks every component as needing an administrator (auth="root"), which makes
    # Installer ask for a password even for a home-folder install. pkgbuild has no option for it,
    # so the attribute is changed in the expanded component, as the Packages app does for its own
    # setting.
    pkgutil --expand "$work/component-$i.pkg" "$work/component-$i"
    sed -i '' 's/auth="root"/auth="none"/' "$work/component-$i/PackageInfo"
    grep -q 'auth="none"' "$work/component-$i/PackageInfo" ||
        { echo "$name's component still needs an administrator" >&2; exit 1; }
    rm "$work/component-$i.pkg"
    pkgutil --flatten "$work/component-$i" "$work/$identifier.pkg"
    outline+="<line choice=\"$identifier\"/>"
    choices+="<choice id=\"$identifier\" visible=\"true\" start_selected=\"true\" title=\"Sineless ${titles[$i]}\"><pkg-ref id=\"$identifier\"/></choice>"
    choices+="<pkg-ref id=\"$identifier\" version=\"$version\" onConclusion=\"none\">$identifier.pkg</pkg-ref>"
done

cat > "$work/resources/welcome.html" <<'HTML'
<html><body style="font-family: -apple-system; font-size: 13px">
<p>This installs <b>Sineless</b>, an open-source oscilloscope and multi-track analyser by Orizon
Audio, as a VST3 plugin in your <code>~/Library/Audio/Plug-Ins/VST3</code> folder and as an Audio
Unit in your <code>~/Library/Audio/Plug-Ins/Components</code> folder. No password is needed.
Customize lets you leave either out.</p>
<p>After installing, rescan plugins in your host if Sineless does not appear.</p>
</body></html>
HTML
# The home folder only: Installer then installs as the user, into ~ plus each install location,
# without asking where. hostArchitectures with arm64 keeps Installer out of Rosetta.
cat > "$work/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Sineless $version</title>
    <welcome file="welcome.html" mime-type="text/html"/>
    <domains enable_anywhere="false" enable_currentUserHome="true" enable_localSystem="false"/>
    <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <volume-check><allowed-os-versions><os-version min="$minimum"/></allowed-os-versions></volume-check>
    <choices-outline>$outline</choices-outline>
    $choices
</installer-gui-script>
XML
package="build/release/Sineless-$version-macOS.pkg"
rm -f "$package"
productbuild --distribution "$work/distribution.xml" --resources "$work/resources" \
    --package-path "$work" "$package"

# Checks: it installs only into the home folder, and carries each bundle as signed and checked
# above.
domains="$(installer -pkg "$package" -dominfo | tr -d '[:space:]')"
[[ "$domains" == "CurrentUserHomeDirectory" ]] ||
    { echo "The installer offers $domains, not the home folder alone" >&2; exit 1; }
pkgutil --expand-full "$package" "$work/check"
for i in "${!names[@]}"; do
    payload="$work/check/${identifiers[$i]}.pkg/Payload/${names[$i]}"
    codesign --verify --strict "$payload"
    diff -r "${built[$i]}" "$payload"
done

if [[ "${INSTALL_CHECK:-}" == 1 ]]; then
    # As a user meets it: the installer downloaded by a browser, so quarantined.
    downloaded="$work/downloaded.pkg"
    cp "$package" "$downloaded"
    quarantine() { xattr -w com.apple.quarantine "0083;$(printf %x "$(date +%s)");Safari;" "$1"; }
    quarantine "$downloaded"
    plugins="$HOME/Library/Audio/Plug-Ins"
    # Each installed bundle equals the built one, signed, with no quarantine flag on any file.
    check_installed() {
        for i in "${!names[@]}"; do
            installed="$plugins/${folders[$i]}/${names[$i]}"
            codesign --verify --strict "$installed"
            diff -r "${built[$i]}" "$installed"
            if xattr -r "$installed" | grep -q com.apple.quarantine; then
                echo "$installed is quarantined" >&2
                exit 1
            fi
        done
    }
    # A first install, into a home with no VST3 or Components folder yet, as on most Macs. On a CI
    # runner, the ones it has are moved aside meanwhile; elsewhere the check needs there to be
    # none, since moving a user's plug-ins away is not this script's to do.
    move() { mv "$1" "$2" 2>/dev/null || sudo mv "$1" "$2"; }
    for folder in "${folders[@]}"; do
        if [[ -e "$plugins/$folder" && -n "${CI:-}" ]]; then
            move "$plugins/$folder" "$work/$folder-before"
        fi
    done
    absent=true
    for folder in "${folders[@]}"; do
        [[ -e "$plugins/$folder" ]] && absent=false
    done
    if [[ "$absent" == true ]]; then
        installer -pkg "$downloaded" -target CurrentUserHomeDirectory
        check_installed
        for folder in "${folders[@]}"; do
            [[ -O "$plugins/$folder" ]] || { echo "$plugins/$folder is not owned by the user" >&2; exit 1; }
        done
        echo "Installed and checked both bundles in a home with no VST3 or Components folder"
    else
        echo "Skipped the first install: a VST3 or Components folder exists"
    fi
    for folder in "${folders[@]}"; do
        if [[ -e "$work/$folder-before" ]]; then
            rm -rf "${plugins:?}/$folder"
            move "$work/$folder-before" "$plugins/$folder"
        fi
    done
    # An upgrade, over an older Sineless from the zip, still quarantined and holding a file the new
    # one lacks. Its binary differs from the new one, as an older release's would, so the check
    # proves the upgrade replaces it.
    for i in "${!names[@]}"; do
        installed="$plugins/${folders[$i]}/${names[$i]}"
        mkdir -p "$(dirname "$installed")"
        rm -rf "$installed"
        ditto "${built[$i]}" "$installed"
        touch "$installed/Contents/Resources/left-by-an-older-version"
        printf older >> "$installed/Contents/MacOS/Sineless"
        quarantine "$installed"
    done
    installer -pkg "$downloaded" -target CurrentUserHomeDirectory
    check_installed
    echo "Installed and checked both bundles over older copies"

    # Customize: either format can be left out, and only the other is then installed.
    for left in "${!names[@]}"; do
        for i in "${!names[@]}"; do
            rm -rf "${plugins:?}/${folders[$i]}/${names[$i]}"
        done
        cat > "$work/without-$left.xml" <<CHOICES
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<array>
    <dict>
        <key>attributeSetting</key><integer>0</integer>
        <key>choiceAttribute</key><string>selected</string>
        <key>choiceIdentifier</key><string>${identifiers[$left]}</string>
    </dict>
</array>
</plist>
CHOICES
        installer -pkg "$downloaded" -target CurrentUserHomeDirectory \
            -applyChoiceChangesXML "$work/without-$left.xml"
        for i in "${!names[@]}"; do
            installed="$plugins/${folders[$i]}/${names[$i]}"
            if [[ "$i" == "$left" && -e "$installed" ]]; then
                echo "Customize did not leave out the ${titles[$i]}" >&2
                exit 1
            elif [[ "$i" != "$left" && ! -e "$installed" ]]; then
                echo "Leaving out the ${titles[$left]} left out the ${titles[$i]} as well" >&2
                exit 1
            fi
        done
        echo "Installed without the ${titles[$left]}: only the other format arrived"
    done
    # Both formats again, so that a run outside CI ends with this build installed.
    installer -pkg "$downloaded" -target CurrentUserHomeDirectory
    check_installed
fi
echo "Packaged $package ($(du -h "$package" | cut -f1))"
