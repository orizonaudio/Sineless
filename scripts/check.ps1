# Runs every check that applies on Windows. Stops at the first failure.
#
# The sanitizer builds, clang-tidy and the performance limits run on macOS (scripts/check.sh),
# which shares all of this code.
#
# Environment:
#   PRESETS   Windows presets to build, comma-separated (default: windows-x64,windows-arm64)
#   LLVM_BIN  Directory with clang-format (default: C:\Program Files\LLVM\bin)
#   RUN_BENCH 1 to run the audio benchmark here; it runs by default on GitHub's machines only,
#             since its limit means something on real x64 hardware alone (not under emulation)
#   SKIP_PERFORMANCE  1 to skip the performance limits (scripts\performance.ps1)
#   SKIP_FORMAT       1 to skip the C++ format check (CI runs it once, with a pinned clang-format)
#   SKIP_APP_TESTS    1 to skip the app tests in WebKit and Chromium (CI runs them once, on Linux)
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

$presets = if ($env:PRESETS) { $env:PRESETS -split ',' } else { @('windows-x64', 'windows-arm64') }
$llvmBin = if ($env:LLVM_BIN) { $env:LLVM_BIN } else { 'C:\Program Files\LLVM\bin' }

function Step($name) { Write-Host "`n==> $name" }

Step 'Toolchain'
. (Join-Path $PSScriptRoot 'windows-env.ps1')
# Windows PowerShell 5.1 strips double quotes inside arguments to programs: single quotes only.
$nodeMajor = [int](node -p "process.versions.node.split('.')[0]")
if ($nodeMajor -lt 22) { throw "Node $(node -v) is too old; install the version in .nvmrc." }
if ($env:SKIP_FORMAT -ne '1') {
    if (-not (Test-Path "$llvmBin\clang-format.exe")) { throw "Missing $llvmBin\clang-format.exe (winget install LLVM.LLVM)." }

    Step 'C++ format'
    $sources = git ls-files -co --exclude-standard -- '*.cpp' '*.h' '*.mm'
    Run "$llvmBin\clang-format.exe" --dry-run --Werror @sources
}

if (-not (Test-Path node_modules)) {
    Step 'npm packages'
    Run npm.cmd ci --no-audit --no-fund
}

foreach ($preset in $presets) {
    Step "C++ build: $preset (includes the VST3 validator)"
    Run cmake --preset $preset
    Run cmake --build --preset $preset -- /m /nologo /v:minimal

    if ($preset -like '*arm64' -and -not $hostIsArm) {
        Write-Host "Tests skipped: this machine cannot run $preset binaries."
        continue
    }
    Step "C++ tests: $preset"
    Run ctest --preset $preset
}

Step 'Bundle with both architectures'
# Each preset builds a bundle with its own Contents\<arch>-win folder; one bundle holds both.
$bundle = 'build\windows\Sineless.vst3'
if (Test-Path $bundle) { Remove-Item -Recurse -Force $bundle }
foreach ($preset in $presets) {
    # robocopy merges folder trees; its exit codes below 8 mean success.
    robocopy "build\$preset\VST3\Debug\Sineless.vst3" $bundle /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Could not copy the $preset bundle." }
}
$global:LASTEXITCODE = 0
Get-ChildItem "$bundle\Contents" -Directory | ForEach-Object { Write-Host "  $($_.Name)" }

Step 'pluginval (x64)'
# pluginval runs on x64 only; on ARM64 Windows it runs emulated and validates the x64 build.
$pluginvalVersion = 'v1.0.4'
$pluginvalSha256 = 'C08E61CE3B96DB41636F8EC7E76F4C7E2C13EBDAC7FA1B5A1F52B4F32EC715AB'
$pluginval = 'build\tools\pluginval.exe'
if (-not (Test-Path $pluginval)) {
    New-Item -ItemType Directory -Force build\tools | Out-Null
    $zip = 'build\tools\pluginval_Windows.zip'
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -UseBasicParsing -OutFile $zip `
        -Uri "https://github.com/Tracktion/pluginval/releases/download/$pluginvalVersion/pluginval_Windows.zip"
    if ((Get-FileHash -Algorithm SHA256 $zip).Hash -ne $pluginvalSha256) { throw 'pluginval download has the wrong checksum.' }
    Expand-Archive -Force $zip build\tools
}
if ($presets -contains 'windows-x64') {
    Run $pluginval --strictness-level 10 --validate-in-process --validate 'build\windows-x64\VST3\Debug\Sineless.vst3'
}

if ($env:SKIP_PERFORMANCE -eq '1') {
    Write-Host "`nPerformance limits skipped (SKIP_PERFORMANCE=1)."
} else {
    & (Join-Path $PSScriptRoot 'performance.ps1')
}

Step 'TypeScript'
# npm.cmd, not npm: PowerShell would pick npm.ps1, which mishandles forwarded arguments.
Run npm.cmd run typecheck
Run npm.cmd run lint
Run npm.cmd run format:check
Run npm.cmd test
if ($env:SKIP_APP_TESTS -ne '1') {
    Run npx.cmd playwright install webkit chromium
    Run npm.cmd run test:app
}
Run npm.cmd run build

Write-Host "`nAll checks passed."
