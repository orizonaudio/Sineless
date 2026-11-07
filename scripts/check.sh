#!/usr/bin/env bash
# Runs every check. Every stage runs whatever another finds, and a summary at the end lists the
# failures, each with the command that reruns it alone where there is one (scripts/stages.sh); a
# stage that needs another (a build's tests) is skipped when that one failed. Exits non-zero if any
# stage failed.
#
# Three parts, all by default; CI runs each as a job of its own:
#   main   format, build, tests, the VST3 validator, pluginval, the Audio Unit's validation
#          (auval and pluginval, on a copy installed for this user), performance, TypeScript
#   glue   the macOS editor's web view tests and the Audio Unit's view tests under
#          AddressSanitizer and ThreadSanitizer, the Audio Unit's other tests under AddressSanitizer
#          and RealtimeSanitizer, and clang-tidy over the macOS sources
#   linux  the shared code's checks, scripts/check-linux.sh (in Docker here)
#
# The order: the checks that need no build first (C++ format, the TypeScript type check, lint and
# format, the macOS and Linux lint), so that their findings show within minutes; then the builds
# and tests of each part; the performance limits last, on a machine the other stages no longer
# load.
#
# Environment:
#   CHECKS    Parts to run, space-separated (default: "main glue linux")
#   PRESET    CMake preset to use (default: dev-local if CMakeUserPresets.json exists, else dev)
#   LLVM_BIN  Directory with clang-format, clang-tidy and the Clang that builds RealtimeSanitizer
#             (default: Homebrew LLVM)
#   SKIP_PERFORMANCE=1  Skip the performance limits (they need a logged-in desktop session)
#   SKIP_FORMAT=1       Skip the C++ format check (CI runs it once, with a pinned clang-format)
#   SKIP_APP_TESTS=1    Skip the app tests in WebKit and Chromium (CI runs them once, on macOS)
#   SKIP_TYPESCRIPT=1   Skip the TypeScript checks (CI's typescript job runs them)
#   BUILD_ONLY=1        Run the builds alone, to fill the compiler caches
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

if [[ -z "${PRESET:-}" ]]; then
    if [[ -f CMakeUserPresets.json ]]; then PRESET=dev-local; else PRESET=dev; fi
fi
LLVM_BIN="${LLVM_BIN:-$(brew --prefix llvm 2>/dev/null)/bin}"
buildDir="build/dev"
jobs="$(sysctl -n hw.ncpu)"

pluginvalVersion="v1.0.4"
pluginvalSha256="3c4c533bda0c5059eea3ddaea752d757ee2025041f0f47e6bcb0e87f6082b29f"
toolsDir="build/tools"
pluginval="$toolsDir/pluginval.app/Contents/MacOS/pluginval"

checks=" ${CHECKS:-main glue linux} "
part() { [[ "$checks" == *" $1 "* ]]; }
typescript() { part main && [[ "${SKIP_TYPESCRIPT:-}" != 1 ]]; }

# The toolchain is checked before any stage: without it none can run.
printf '\n==> Toolchain\n'
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
if typescript && [[ ! -d node_modules ]]; then
    npm ci
fi

source scripts/stages.sh

format() {
    git ls-files -co --exclude-standard -- '*.cpp' '*.h' '*.mm' ':!docs/' |
        xargs "$LLVM_BIN/clang-format" --dry-run --Werror
}

macos_lint() {
    # The macOS sources only (the web view glue, the Audio Unit, and the Objective-C++ tests,
    # benchmark and showcase); Linux lints the shared ones. With the AddressSanitizer build's
    # commands: arm64 only (clang-tidy cannot handle a universal binary's two -arch flags). It
    # configures the benchmark and showcase (ORIZON_BENCH) without building them, so that they are
    # linted with their own flags, not a neighbour's. Of the generated headers the sources include,
    # configuring writes all but the protocol's, which is built here.
    cmake --preset asan
    cmake --build --preset asan --target protocol_header
    git ls-files -co --exclude-standard -- 'editor/src/mac/*' 'editor/*.mm' 'au/*.cpp' 'au/*.mm' |
        xargs -P "$jobs" -n 4 \
            "$LLVM_BIN/clang-tidy" -p build/asan --quiet --extra-arg="-isysroot$(xcrun --show-sdk-path)"
}

build() {
    cmake --preset "$@"
    cmake --build --preset "$1" -j "$jobs"
}

# pluginval, downloaded once into build/tools; the pluginval and Audio Unit stages both run it.
pluginval_ready() {
    if [[ ! -x "$pluginval" ]]; then
        mkdir -p "$toolsDir"
        curl -sSL -o "$toolsDir/pluginval_macOS.zip" \
            "https://github.com/Tracktion/pluginval/releases/download/$pluginvalVersion/pluginval_macOS.zip"
        echo "$pluginvalSha256  $toolsDir/pluginval_macOS.zip" | shasum -a 256 -c -
        unzip -oq "$toolsDir/pluginval_macOS.zip" -d "$toolsDir"
    fi
}

validate_vst3() {
    pluginval_ready
    "$pluginval" --strictness-level 10 --validate-in-process \
        --validate "$buildDir/VST3/Debug/Sineless.vst3"
}

validate_au() {
    pluginval_ready
    # Hosts and the validators find Audio Units only once installed, so this build is installed
    # in ~/Library/Audio/Plug-Ins/Components, where it stays until replaced.
    scripts/validate-au.sh "$buildDir/lib/Debug/Sineless.component" "$pluginval"
}

app_tests() {
    npx playwright install webkit chromium
    npm run test:app
}

# The checks that need no build.
if part main && [[ "${SKIP_FORMAT:-}" != 1 ]]; then
    stage "C++ format" -- format
fi
if typescript; then
    stage "TypeScript typecheck" --rerun "npm run typecheck" -- npm run typecheck
    stage "TypeScript lint" --rerun "npm run lint" -- npm run lint
    stage "TypeScript format" --rerun "npm run format:check" -- npm run format:check
fi
if part glue; then
    stage "macOS lint" -- macos_lint
fi
if part linux; then
    stage "Linux lint" --rerun "scripts/check-linux.sh lint" -- scripts/check-linux.sh lint
fi

if part main; then
    stage "C++ build" --build -- build "$PRESET"
    stage "C++ tests" --needs "C++ build" -- ctest --preset "$PRESET" -j "$jobs"
    stage "pluginval" --needs "C++ build" -- validate_vst3
    stage "Audio Unit validation" --needs "C++ build" -- validate_au
    if typescript; then
        stage "TypeScript tests" --rerun "npm test" -- npm test
        if [[ "${SKIP_APP_TESTS:-}" != 1 ]]; then
            stage "App tests" --rerun "npm run test:app" -- app_tests
        fi
        stage "Page build" --rerun "npm run build" -- npm run build
    fi
fi

if part glue; then
    # The macOS glue: memory errors (AddressSanitizer), concurrency (ThreadSanitizer), and the
    # real-time safety of the Audio Unit (RealtimeSanitizer; the shared code's render path is
    # checked on Linux, where the Audio Unit is not built). AppleClang has no RealtimeSanitizer:
    # LLVM's Clang builds it.
    stage "asan build" --build -- build asan
    stage "asan tests" --needs "asan build" -- env ASAN_OPTIONS=halt_on_error=1 ctest --preset asan
    stage "tsan build" --build -- build tsan
    stage "tsan tests" --needs "tsan build" -- env TSAN_OPTIONS=halt_on_error=1 ctest --preset tsan
    stage "rtsan build" --build -- build rtsan -DCMAKE_CXX_COMPILER="$LLVM_BIN/clang++"
    stage "rtsan tests" --needs "rtsan build" -- ctest --preset rtsan
fi

if part linux; then
    stage "Linux sanitizers" --build --rerun "scripts/check-linux.sh asan tsan rtsan" -- \
        scripts/check-linux.sh asan tsan rtsan
fi

if part main && [[ "${SKIP_PERFORMANCE:-}" != 1 ]]; then
    stage "Performance limits" --rerun "scripts/performance.sh" -- scripts/performance.sh
fi

stages_report
