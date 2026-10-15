#!/usr/bin/env bash
# Runs every check. Exits non-zero on the first failure.
#
# Environment:
#   PRESET    CMake preset to use (default: dev-local if CMakeUserPresets.json exists, else dev)
#   LLVM_BIN  Directory with clang-format and clang-tidy (default: Homebrew LLVM)
#   SKIP_PERFORMANCE=1  Skip the performance limits (they need a logged-in desktop session)
#   SKIP_FORMAT=1       Skip the C++ format check (CI runs it once, with a pinned clang-format)
#   SKIP_APP_TESTS=1    Skip the app tests in WebKit and Chromium (CI runs them once, on Linux)
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

if [[ "${SKIP_FORMAT:-}" != 1 ]]; then
    step "C++ format"
    git ls-files -co --exclude-standard -- '*.cpp' '*.h' '*.mm' |
        xargs "$LLVM_BIN/clang-format" --dry-run --Werror
fi

step "C++ build (includes the VST3 validator)"
cmake --preset "$PRESET"
cmake --build --preset "$PRESET" -j "$(sysctl -n hw.ncpu)"

step "C++ tests"
ctest --preset "$PRESET" -j "$(sysctl -n hw.ncpu)"

step "Real-time safety (RealtimeSanitizer build)"
cmake --preset rtsan
cmake --build --preset rtsan -j "$(sysctl -n hw.ncpu)"
ctest --preset rtsan -j "$(sysctl -n hw.ncpu)"

step "Concurrency (ThreadSanitizer build)"
cmake --preset tsan
cmake --build --preset tsan -j "$(sysctl -n hw.ncpu)"
TSAN_OPTIONS=halt_on_error=1 ctest --preset tsan -j "$(sysctl -n hw.ncpu)"

step "Memory errors (AddressSanitizer build)"
cmake --preset asan
cmake --build --preset asan -j "$(sysctl -n hw.ncpu)"
ASAN_OPTIONS=halt_on_error=1 ctest --preset asan -j "$(sysctl -n hw.ncpu)"

step "C++ lint"
# clang-tidy cannot handle universal-binary commands (two -arch flags); lint the arm64 slice.
mkdir -p build/lint
sed 's/ -arch x86_64//g' "$buildDir/compile_commands.json" > build/lint/compile_commands.json
# Windows-only sources need Windows headers: they are checked by the Windows build. The fuzzers
# build only in the Linux fuzz preset, whose CI job checks them. One clang-tidy per core, a few
# files each; xargs fails if any of them does.
git ls-files -co --exclude-standard -- 'engine/*.cpp' 'editor/*.cpp' 'editor/*.mm' 'plugin/*.cpp' \
    ':!editor/src/win/*' ':!editor/tests/*Win.cpp' ':!engine/tests/fuzz/*' |
    xargs -P "$(sysctl -n hw.ncpu)" -n 4 \
        "$LLVM_BIN/clang-tidy" -p build/lint --quiet --extra-arg="-isysroot$(xcrun --show-sdk-path)"

step "pluginval"
if [[ ! -x "$pluginval" ]]; then
    mkdir -p "$toolsDir"
    curl -sSL -o "$toolsDir/pluginval_macOS.zip" \
        "https://github.com/Tracktion/pluginval/releases/download/$pluginvalVersion/pluginval_macOS.zip"
    echo "$pluginvalSha256  $toolsDir/pluginval_macOS.zip" | shasum -a 256 -c -
    unzip -oq "$toolsDir/pluginval_macOS.zip" -d "$toolsDir"
fi
"$pluginval" --strictness-level 10 --validate-in-process \
    --validate "$buildDir/VST3/Debug/Sineless.vst3"

if [[ "${SKIP_PERFORMANCE:-}" != 1 ]]; then
    scripts/performance.sh
fi

step "TypeScript"
[[ -d node_modules ]] || npm ci
npm run typecheck
npm run lint
npm run format:check
npm test
if [[ "${SKIP_APP_TESTS:-}" != 1 ]]; then
    npx playwright install webkit chromium
    npm run test:app
fi
npm run build

printf '\nAll checks passed.\n'
