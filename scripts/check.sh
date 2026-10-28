#!/usr/bin/env bash
# Runs every check. Exits non-zero on the first failure.
#
# Three parts, all by default; CI runs each as a job of its own:
#   main   format, build, tests, the VST3 validator, pluginval, the Audio Unit's validation
#          (auval and pluginval, on a copy installed for this user), performance, TypeScript
#   glue   the macOS editor's web view tests and the Audio Unit's view tests under
#          AddressSanitizer and ThreadSanitizer, the Audio Unit's other tests under AddressSanitizer
#          and RealtimeSanitizer, and clang-tidy over the macOS sources
#   linux  the shared code's checks, scripts/check-linux.sh (in Docker here)
#
# Environment:
#   CHECKS    Parts to run, space-separated (default: "main glue linux")
#   PRESET    CMake preset to use (default: dev-local if CMakeUserPresets.json exists, else dev)
#   LLVM_BIN  Directory with clang-format, clang-tidy and the Clang that builds RealtimeSanitizer
#             (default: Homebrew LLVM)
#   SKIP_PERFORMANCE=1  Skip the performance limits (they need a logged-in desktop session)
#   SKIP_FORMAT=1       Skip the C++ format check (CI runs it once, with a pinned clang-format)
#   SKIP_APP_TESTS=1    Skip the app tests in WebKit and Chromium (CI runs them once, on Linux)
#   SKIP_TYPESCRIPT=1   Skip the TypeScript checks (CI's typescript job runs them)
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

checks=" ${CHECKS:-main glue linux} "
part() { [[ "$checks" == *" $1 "* ]]; }

step "Toolchain"
# The floor is package.json's engines field, as the C++ build reads it.
if ! node -e '
    const floor = require("./package.json").engines.node.replace(">=", "").split(".").map(Number);
    const have = process.versions.node.split(".").map(Number);
    const i = have.findIndex((part, index) => part !== floor[index]);
    process.exit(i === -1 || have[i] > floor[i] ? 0 : 1);'; then
    echo "Node $(node -v) is too old; package.json needs $(node -p 'require("./package.json").engines.node') (nvm install 22, for the newest 22)." >&2
    exit 1
fi
for tool in clang-format clang-tidy clang++; do
    [[ -x "$LLVM_BIN/$tool" ]] || { echo "Missing $LLVM_BIN/$tool (brew install llvm)." >&2; exit 1; }
done

if part main; then
    if [[ "${SKIP_FORMAT:-}" != 1 ]]; then
        step "C++ format"
        git ls-files -co --exclude-standard -- '*.cpp' '*.h' '*.mm' ':!docs/' |
            xargs "$LLVM_BIN/clang-format" --dry-run --Werror
    fi

    step "C++ build (includes the VST3 validator)"
    cmake --preset "$PRESET"
    cmake --build --preset "$PRESET" -j "$(sysctl -n hw.ncpu)"

    step "C++ tests"
    ctest --preset "$PRESET" -j "$(sysctl -n hw.ncpu)"

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

    step "Audio Unit: auval and pluginval"
    # Hosts and the validators find Audio Units only once installed, so this build is installed
    # in ~/Library/Audio/Plug-Ins/Components, where it stays until replaced.
    scripts/validate-au.sh "$buildDir/lib/Debug/Sineless.component" "$pluginval"

    if [[ "${SKIP_PERFORMANCE:-}" != 1 ]]; then
        scripts/performance.sh
    fi

    if [[ "${SKIP_TYPESCRIPT:-}" != 1 ]]; then
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
    fi
fi

if part glue; then
    step "macOS glue: memory errors (AddressSanitizer build)"
    cmake --preset asan
    cmake --build --preset asan -j "$(sysctl -n hw.ncpu)"
    ASAN_OPTIONS=halt_on_error=1 ctest --preset asan

    step "macOS glue: concurrency (ThreadSanitizer build)"
    cmake --preset tsan
    cmake --build --preset tsan -j "$(sysctl -n hw.ncpu)"
    TSAN_OPTIONS=halt_on_error=1 ctest --preset tsan

    step "macOS glue: real-time safety of the Audio Unit (RealtimeSanitizer build)"
    # The shared code's render path is checked on Linux, where the Audio Unit is not built.
    # AppleClang has no RealtimeSanitizer: LLVM's Clang builds it.
    cmake --preset rtsan -DCMAKE_CXX_COMPILER="$LLVM_BIN/clang++"
    cmake --build --preset rtsan -j "$(sysctl -n hw.ncpu)"
    ctest --preset rtsan

    step "macOS glue: C++ lint"
    # The macOS sources only (the web view glue, the Audio Unit, and the Objective-C++ tests,
    # benchmark and showcase); Linux lints the shared ones. With the AddressSanitizer build's
    # commands: arm64 only (clang-tidy cannot handle a universal binary's two -arch flags), and
    # its generated headers are built. It configures the benchmark and showcase (ORIZON_BENCH)
    # without building them, so that they are linted with their own flags, not a neighbour's.
    git ls-files -co --exclude-standard -- 'editor/src/mac/*' 'editor/*.mm' 'au/*.cpp' 'au/*.mm' |
        xargs -P "$(sysctl -n hw.ncpu)" -n 4 \
            "$LLVM_BIN/clang-tidy" -p build/asan --quiet --extra-arg="-isysroot$(xcrun --show-sdk-path)"
fi

if part linux; then
    step "Linux: the shared code's checks"
    scripts/check-linux.sh
fi

printf '\nAll checks passed.\n'
