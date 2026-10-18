#!/usr/bin/env bash
# Installs Mull for Homebrew's LLVM 20 on macOS (Apple silicon) into a folder, checked against the
# checksum GitHub publishes for the release: for scripts/mutation-au.sh, on a Mac and on CI. The
# same Mull version as tools/mutation/install-mull.sh installs on Linux. Its Clang plugin links
# Homebrew's llvm@20 (brew install llvm@20), and its macOS builds need macOS 26.5 or later.
#   tools/mutation/install-mull-macos.sh <folder>
set -euo pipefail

version=0.34.1
sum=61083bed130cd196a442307c29dd734cd393a145c0b5fc5311c5d6172fcb9f70
dir="${1:?Usage: tools/mutation/install-mull-macos.sh <folder>}"
download="$(mktemp -d)"
curl -fsSL -o "$download/mull.zip" \
    "https://github.com/mull-project/mull/releases/download/$version/Mull-20-$version-LLVM-20.1.8-macOS-aarch64-26.6.2.zip"
echo "$sum  $download/mull.zip" | shasum -a 256 -c - > /dev/null
rm -rf "$dir"
mkdir -p "$dir"
unzip -q "$download/mull.zip" -d "$dir"
rm -rf "$download"
