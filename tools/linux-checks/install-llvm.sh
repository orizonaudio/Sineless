#!/usr/bin/env bash
# Installs LLVM 23's Clang, clang-tidy, sanitizer runtimes and tools from apt.llvm.org on Ubuntu
# 24.04, for CI's Linux jobs and tools/linux-checks/Dockerfile alike, so that both check with the
# same LLVM. The tools include llvm-symbolizer, without which a sanitizer's report names no
# functions and its suppressions match nothing. Ubuntu 24.04's own Clang is 18, older than
# RealtimeSanitizer (LLVM 20) and than the clang-tidy macOS lints with (Homebrew's LLVM). The
# version matches the pinned clang-format (.github/workflows/ci.yml) and the presets' clang-23
# (CMakePresets.json, linux-checks).
set -euo pipefail
llvm=23
sudo=$([[ "$(id -u)" == 0 ]] || echo sudo)
$sudo apt-get update
DEBIAN_FRONTEND=noninteractive $sudo apt-get install -y --no-install-recommends curl ca-certificates gnupg
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | $sudo gpg --dearmor --yes -o /usr/share/keyrings/llvm.gpg
echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-$llvm main" |
    $sudo tee /etc/apt/sources.list.d/llvm.list > /dev/null
$sudo apt-get update
DEBIAN_FRONTEND=noninteractive $sudo apt-get install -y --no-install-recommends \
    "clang-$llvm" "clang-tidy-$llvm" "libclang-rt-$llvm-dev" "llvm-$llvm"
