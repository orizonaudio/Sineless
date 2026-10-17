#!/usr/bin/env bash
# Builds the release bundle, zips it and wraps it in an installer for GitHub Releases. Neither has
# a Developer ID signature; the bundle is signed ad hoc, which seals the whole bundle (the linker
# only signs the binary).
#
# The installer puts the bundle in the user's own ~/Library/Audio/Plug-Ins/VST3 without asking for
# a password, and what Installer writes carries no quarantine flag, so unlike the zip it needs no
# Terminal step. With INSTALL_CHECK=1 (the release workflow, on a runner thrown away afterwards) it
# is also installed and the installed bundle checked; `installer` ignores HOME, so that installs
# into the real home folder.
#
# Output: build/release/Sineless-<version>-macOS.zip and build/release/Sineless-<version>-macOS.pkg
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

cmake --preset release
cmake --build --preset release -j "$(sysctl -n hw.ncpu)"

bundle="build/release/VST3/Release/Sineless.vst3"
binary="$bundle/Contents/MacOS/Sineless"
version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$bundle/Contents/Info.plist")"

# Checks of C-2: both architectures, macOS 13.1 minimum.
for arch in arm64 x86_64; do
    lipo "$binary" -verify_arch "$arch" || { echo "Missing $arch slice" >&2; exit 1; }
done
minimum="$(vtool -show-build "$binary" | awk '/minos/ { print $2; exit }')"
[[ "$minimum" == "13.1" ]] || { echo "Minimum macOS is $minimum, expected 13.1" >&2; exit 1; }

codesign --force --sign - "$bundle"
codesign --verify --strict "$bundle"

archive="build/release/Sineless-$version-macOS.zip"
rm -f "$archive"
ditto -c -k --keepParent --norsrc --noextattr "$bundle" "$archive"
echo "Packaged $archive ($(du -h "$archive" | cut -f1))"

# The installer: one component, the bundle, built from a folder holding only it.
work="build/release/pkg"
rm -rf "$work"
mkdir -p "$work/root" "$work/resources"
ditto "$bundle" "$work/root/Sineless.vst3"
# Not relocatable: Installer would otherwise "upgrade" a Sineless.vst3 it finds elsewhere, such as
# a build folder, instead of installing into the VST3 folder. Upgrade replaces the whole bundle.
cat > "$work/component.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<array>
    <dict>
        <key>RootRelativeBundlePath</key><string>Sineless.vst3</string>
        <key>BundleIsRelocatable</key><false/>
        <key>BundleIsVersionChecked</key><false/>
        <key>BundleOverwriteAction</key><string>upgrade</string>
    </dict>
</array>
</plist>
PLIST
pkgbuild --root "$work/root" --component-plist "$work/component.plist" \
    --identifier com.orizonaudio.sineless.vst3 --version "$version" \
    --install-location /Library/Audio/Plug-Ins/VST3 "$work/component.pkg"
# pkgbuild marks every component as needing an administrator (auth="root"), which makes Installer
# ask for a password even for a home-folder install. pkgbuild has no option for it, so the
# attribute is changed in the expanded component, as the Packages app does for its own setting.
pkgutil --expand "$work/component.pkg" "$work/component"
sed -i '' 's/auth="root"/auth="none"/' "$work/component/PackageInfo"
grep -q 'auth="none"' "$work/component/PackageInfo" ||
    { echo "The component still needs an administrator" >&2; exit 1; }
rm "$work/component.pkg"
pkgutil --flatten "$work/component" "$work/Sineless-vst3.pkg"

cat > "$work/resources/welcome.html" <<'HTML'
<html><body style="font-family: -apple-system; font-size: 13px">
<p>This installs <b>Sineless</b>, an open-source VST3 oscilloscope by Orizon Audio, in your
<code>~/Library/Audio/Plug-Ins/VST3</code> folder. No password is needed.</p>
<p>After installing, rescan plugins in your host if Sineless does not appear.</p>
</body></html>
HTML
# The home folder only: Installer then installs as the user, into ~ plus the install location,
# without asking where. hostArchitectures with arm64 keeps Installer out of Rosetta.
cat > "$work/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Sineless $version</title>
    <welcome file="welcome.html" mime-type="text/html"/>
    <domains enable_anywhere="false" enable_currentUserHome="true" enable_localSystem="false"/>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <volume-check><allowed-os-versions><os-version min="$minimum"/></allowed-os-versions></volume-check>
    <choices-outline><line choice="default"/></choices-outline>
    <choice id="default" title="Sineless VST3"><pkg-ref id="com.orizonaudio.sineless.vst3"/></choice>
    <pkg-ref id="com.orizonaudio.sineless.vst3" version="$version" onConclusion="none">Sineless-vst3.pkg</pkg-ref>
</installer-gui-script>
XML
package="build/release/Sineless-$version-macOS.pkg"
rm -f "$package"
productbuild --distribution "$work/distribution.xml" --resources "$work/resources" \
    --package-path "$work" "$package"

# Checks: it installs only into the home folder, and carries the bundle as signed and checked above.
domains="$(installer -pkg "$package" -dominfo | tr -d '[:space:]')"
[[ "$domains" == "CurrentUserHomeDirectory" ]] ||
    { echo "The installer offers $domains, not the home folder alone" >&2; exit 1; }
pkgutil --expand-full "$package" "$work/check"
payload="$work/check/Sineless-vst3.pkg/Payload/Sineless.vst3"
codesign --verify --strict "$payload"
diff -r "$bundle" "$payload"

if [[ "${INSTALL_CHECK:-}" == 1 ]]; then
    # As a user meets it: the installer downloaded by a browser, so quarantined, and installed over
    # an older Sineless from the zip, still quarantined and holding a file the new one lacks.
    downloaded="$work/downloaded.pkg"
    cp "$package" "$downloaded"
    xattr -w com.apple.quarantine "0083;$(printf %x "$(date +%s)");Safari;" "$downloaded"
    installed="$HOME/Library/Audio/Plug-Ins/VST3/Sineless.vst3"
    mkdir -p "$(dirname "$installed")"
    rm -rf "$installed"
    ditto "$bundle" "$installed"
    touch "$installed/Contents/Resources/left-by-an-older-version"
    xattr -w com.apple.quarantine "0083;$(printf %x "$(date +%s)");Safari;" "$installed"
    installer -pkg "$downloaded" -target CurrentUserHomeDirectory
    codesign --verify --strict "$installed"
    diff -r "$bundle" "$installed"
    if xattr -r "$installed" | grep -q com.apple.quarantine; then
        echo "The installed bundle is quarantined" >&2
        exit 1
    fi
    echo "Installed and checked $installed"
fi
echo "Packaged $package ($(du -h "$package" | cut -f1))"
