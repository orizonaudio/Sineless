#!/usr/bin/env bash
# Installs Mull for Ubuntu 24.04's Clang 18, checked against its published checksum: on CI and in
# tools/mutation/Dockerfile. Run as root (or with sudo).
set -euo pipefail

version=0.34.1
case "$(dpkg --print-architecture)" in
amd64) arch=amd64 sum=983ea9c84ecab621414d5442a66c1a1ce4127491b79d42fd5e2fd6fe01d02699 ;;
arm64) arch=aarch64 sum=598d17a5b5304f5eae7492bd4269e9f5a57dcffc1c4a40af75336c9a8a1ef5ad ;;
*) echo "No Mull package for $(dpkg --print-architecture)." >&2; exit 1 ;;
esac
deb="$(mktemp -d)/mull.deb"
curl -fsSL -o "$deb" \
    "https://github.com/mull-project/mull/releases/download/$version/Mull-18-$version-LLVM-18.1.3-ubuntu-$arch-24.04.deb"
echo "$sum  $deb" | sha256sum -c -
apt-get update
apt-get install -y "$deb"
rm -rf "$(dirname "$deb")"
