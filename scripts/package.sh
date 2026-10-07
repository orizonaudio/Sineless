#!/usr/bin/env bash
# Builds the release bundle and zips it for GitHub Releases. It has no Developer ID
# signature; it is signed ad hoc, which seals the whole bundle (the linker only signs the binary).
#
# Output: build/release/O-Scope-<version>-macOS.zip
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

cmake --preset release
cmake --build --preset release -j "$(sysctl -n hw.ncpu)"

bundle="build/release/VST3/Release/O-Scope.vst3"
binary="$bundle/Contents/MacOS/O-Scope"
version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$bundle/Contents/Info.plist")"

# Checks of C-2: both architectures, macOS 13 minimum.
for arch in arm64 x86_64; do
    lipo "$binary" -verify_arch "$arch" || { echo "Missing $arch slice" >&2; exit 1; }
done
minimum="$(vtool -show-build "$binary" | awk '/minos/ { print $2; exit }')"
[[ "$minimum" == "13.0" ]] || { echo "Minimum macOS is $minimum, expected 13.0" >&2; exit 1; }

codesign --force --sign - "$bundle"
codesign --verify --strict "$bundle"

archive="build/release/O-Scope-$version-macOS.zip"
rm -f "$archive"
ditto -c -k --keepParent --norsrc --noextattr "$bundle" "$archive"
echo "Packaged $archive ($(du -h "$archive" | cut -f1))"
