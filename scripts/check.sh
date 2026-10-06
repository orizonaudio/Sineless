#!/usr/bin/env bash
# Runs every check in DESIGN 8.2. Exits non-zero on the first failure.
#
# Environment:
#   PRESET    CMake preset to use (default: dev-local if CMakeUserPresets.json exists, else dev)
#   LLVM_BIN  Directory with clang-format and clang-tidy (default: Homebrew LLVM)
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

if [[ -z "${PRESET:-}" ]]; then
    if [[ -f CMakeUserPresets.json ]]; then PRESET=dev-local; else PRESET=dev; fi
fi
LLVM_BIN="${LLVM_BIN:-$(brew --prefix llvm 2>/dev/null)/bin}"
buildDir="build/dev"

pluginvalVersion="v1.0.4"
pluginvalSha256="3c4c533bda0c5059eea3ddaea752d757ee2025041f0f47e6bcb0e87f6082b29f"
toolsDir="build/tools"
pluginval="$toolsDir/pluginval.app/Contents/MacOS/pluginval"

step() { printf '\n==> %s\n' "$1"; }

step "Toolchain"
nodeMajor="$(node -p 'process.versions.node.split(".")[0]')"
if (( nodeMajor < 22 )); then
    echo "Node $(node -v) is too old; use the version in .nvmrc (nvm use)." >&2
    exit 1
fi
for tool in clang-format clang-tidy; do
    [[ -x "$LLVM_BIN/$tool" ]] || { echo "Missing $LLVM_BIN/$tool (brew install llvm)." >&2; exit 1; }
done

step "C++ format"
git ls-files -co --exclude-standard -- '*.cpp' '*.h' '*.mm' |
    xargs "$LLVM_BIN/clang-format" --dry-run --Werror

step "C++ build (includes the VST3 validator)"
cmake --preset "$PRESET"
cmake --build --preset "$PRESET" -j "$(sysctl -n hw.ncpu)"

step "C++ tests"
ctest --preset "$PRESET"

step "C++ lint"
# clang-tidy cannot handle universal-binary commands (two -arch flags); lint the arm64 slice.
mkdir -p build/lint
sed 's/ -arch x86_64//g' "$buildDir/compile_commands.json" > build/lint/compile_commands.json
git ls-files -co --exclude-standard -- 'engine/*.cpp' 'editor/*.cpp' 'editor/*.mm' 'plugin/*.cpp' |
    xargs "$LLVM_BIN/clang-tidy" -p build/lint --quiet --extra-arg="-isysroot$(xcrun --show-sdk-path)"

step "pluginval"
if [[ ! -x "$pluginval" ]]; then
    mkdir -p "$toolsDir"
    curl -sSL -o "$toolsDir/pluginval_macOS.zip" \
        "https://github.com/Tracktion/pluginval/releases/download/$pluginvalVersion/pluginval_macOS.zip"
    echo "$pluginvalSha256  $toolsDir/pluginval_macOS.zip" | shasum -a 256 -c -
    unzip -oq "$toolsDir/pluginval_macOS.zip" -d "$toolsDir"
fi
"$pluginval" --strictness-level 10 --validate-in-process \
    --validate "$buildDir/VST3/Debug/Oscilloscope.vst3"

step "TypeScript"
[[ -d node_modules ]] || npm ci
npm run typecheck
npm run lint
npm run format:check
npm test
npm run build

printf '\nAll checks passed.\n'
