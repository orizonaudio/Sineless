# Runs every check that applies on Windows. Every stage runs whatever another finds, and a summary
# at the end lists the failures (scripts\stages.ps1); a stage that needs another (a build's tests)
# is skipped when that one failed. The checks that need no build come first, the performance limit
# last.
#
# Two parts, both by default; CI runs each as a job of its own:
#   main   format, build, tests, the VST3 validator, pluginval, performance, TypeScript
#   glue   the Windows editor's web view tests (WebView2, Win32) under AddressSanitizer
# The shared code's sanitizers and clang-tidy run on Linux (scripts/check-linux.sh).
#
# Environment:
#   CHECKS    Parts to run, space-separated (default: "main glue")
#   PRESETS   Windows presets to build, comma-separated (default: windows-x64; ARM64 is not
#             built for now, but windows-arm64 can be given)
#   LLVM_BIN  Directory with clang-format (default: C:\Program Files\LLVM\bin)
#   RUN_BENCH 1 to run the audio benchmark here; it runs by default on GitHub's machines only,
#             since its limit means something on real x64 hardware alone (not under emulation)
#   SKIP_PERFORMANCE  1 to skip the performance limits (scripts\performance.ps1)
#   SKIP_FORMAT       1 to skip the C++ format check (CI runs it once, with a pinned clang-format)
#   SKIP_APP_TESTS    1 to skip the app tests in WebKit and Chromium (CI runs them once, on macOS)
#   SKIP_TYPESCRIPT   1 to skip the TypeScript checks (CI's typescript job runs them)
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

$presets = if ($env:PRESETS) { $env:PRESETS -split ',' } else { @('windows-x64') }
$llvmBin = if ($env:LLVM_BIN) { $env:LLVM_BIN } else { 'C:\Program Files\LLVM\bin' }
$checks = if ($env:CHECKS) { $env:CHECKS -split ' ' } else { @('main', 'glue') }
$main = $checks -contains 'main'
$typescript = $main -and $env:SKIP_TYPESCRIPT -ne '1'

# The toolchain is checked before any stage: without it none can run.
Write-Host "`n==> Toolchain"
. (Join-Path $PSScriptRoot 'windows-env.ps1')
# Windows PowerShell 5.1 strips double quotes inside arguments to programs: single quotes only.
# The floor is package.json's engines field, as the C++ build reads it.
$nodeFloor = [version]((Get-Content package.json -Raw | ConvertFrom-Json).engines.node -replace '^>=', '')
$nodeVersion = [version]((node -v) -replace '^v', '')
if ($nodeVersion -lt $nodeFloor) { throw "Node $nodeVersion is too old; $nodeFloor or newer is required (install the newest Node 22)." }
if (-not (Test-Path node_modules)) {
    Write-Host "`n==> npm packages"
    Run npm.cmd ci --no-audit --no-fund
}

. (Join-Path $PSScriptRoot 'stages.ps1')

# The checks that need no build.
if ($main -and $env:SKIP_FORMAT -ne '1') {
    Stage -Name 'C++ format' -Body {
        if (-not (Test-Path "$llvmBin\clang-format.exe")) { throw "Missing $llvmBin\clang-format.exe (winget install LLVM.LLVM)." }
        $sources = git ls-files -co --exclude-standard -- '*.cpp' '*.h' '*.mm' ':!docs/'
        Run "$llvmBin\clang-format.exe" --dry-run --Werror @sources
    }
}
if ($typescript) {
    # npm.cmd, not npm: PowerShell would pick npm.ps1, which mishandles forwarded arguments.
    Stage -Name 'TypeScript typecheck' -Rerun 'npm.cmd run typecheck' -Body { Run npm.cmd run typecheck }
    Stage -Name 'TypeScript lint' -Rerun 'npm.cmd run lint' -Body { Run npm.cmd run lint }
    Stage -Name 'TypeScript format' -Rerun 'npm.cmd run format:check' -Body { Run npm.cmd run format:check }
}

if ($main) {
    foreach ($preset in $presets) {
        Stage -Name "C++ build: $preset" -Body {
            Run cmake --preset $preset
            Run cmake --build --preset $preset '--' /m /nologo /v:minimal
        }
        if ($preset -like '*arm64' -and -not $hostIsArm) {
            Write-Host "Tests skipped: this machine cannot run $preset binaries."
        } else {
            Stage -Name "C++ tests: $preset" -Needs "C++ build: $preset" -Body {
                Run ctest --preset $preset -j $env:NUMBER_OF_PROCESSORS
            }
        }
    }

    # Each preset builds a bundle with its own Contents\<arch>-win folder; one bundle holds both.
    Stage -Name 'Bundle' -Needs @($presets | ForEach-Object { "C++ build: $_" }) -Body {
        $bundle = 'build\windows\Sineless.vst3'
        if (Test-Path $bundle) { Remove-Item -Recurse -Force $bundle }
        foreach ($preset in $presets) {
            # robocopy merges folder trees; its exit codes below 8 mean success.
            robocopy "build\$preset\VST3\Release\Sineless.vst3" $bundle /E /NFL /NDL /NJH /NJS /NP | Out-Null
            if ($LASTEXITCODE -ge 8) { throw "Could not copy the $preset bundle." }
        }
        $global:LASTEXITCODE = 0
        Get-ChildItem "$bundle\Contents" -Directory | ForEach-Object { Write-Host "  $($_.Name)" }
    }

    # pluginval runs on x64 only; on ARM64 Windows it runs emulated and validates the x64 build.
    if ($presets -contains 'windows-x64') {
        Stage -Name 'pluginval' -Needs 'Bundle' -Body {
            $pluginvalVersion = 'v1.0.4'
            $pluginvalSha256 = 'C08E61CE3B96DB41636F8EC7E76F4C7E2C13EBDAC7FA1B5A1F52B4F32EC715AB'
            $pluginval = '.\build\tools\pluginval.exe'
            if (-not (Test-Path $pluginval)) {
                New-Item -ItemType Directory -Force build\tools | Out-Null
                $zip = 'build\tools\pluginval_Windows.zip'
                $ProgressPreference = 'SilentlyContinue'
                Invoke-WebRequest -UseBasicParsing -OutFile $zip `
                    -Uri "https://github.com/Tracktion/pluginval/releases/download/$pluginvalVersion/pluginval_Windows.zip"
                if ((Get-FileHash -Algorithm SHA256 $zip).Hash -ne $pluginvalSha256) { throw 'pluginval download has the wrong checksum.' }
                Expand-Archive -Force $zip build\tools
            }
            Run $pluginval --strictness-level 10 --validate-in-process --validate 'build\windows-x64\VST3\Release\Sineless.vst3'
        }
    }

    if ($typescript) {
        Stage -Name 'TypeScript tests' -Rerun 'npm.cmd test' -Body { Run npm.cmd test }
        if ($env:SKIP_APP_TESTS -ne '1') {
            Stage -Name 'App tests' -Rerun 'npm.cmd run test:app' -Body {
                Run npx.cmd playwright install webkit chromium
                Run npm.cmd run test:app
            }
        }
        Stage -Name 'Page build' -Rerun 'npm.cmd run build' -Body { Run npm.cmd run build }
    }
}

if ($checks -contains 'glue' -and $hostIsArm) {
    # An ARM64 machine's developer environment has the ARM64 tools, not the x64 sanitizer
    # runtime the x64 glue build loads; CI runs the glue on x64.
    Write-Host "`nWindows glue skipped: it is checked on x64, and this machine is ARM64."
} elseif ($checks -contains 'glue') {
    # The Windows editor's web view tests (WebView2, Win32) under AddressSanitizer. MSVC's
    # AddressSanitizer has no leak checks; its runtime DLL is found on the PATH that
    # windows-env.ps1 set up (the compiler's folder). The VST3 validator and pluginval run in the
    # main part, on the plain build: they load the plugin into a host not built with the sanitizer.
    Stage -Name 'Glue build' -Body {
        Run cmake --preset windows-x64-glue
        Run cmake --build --preset windows-x64-glue '--' /m /nologo /v:minimal
    }
    Stage -Name 'Glue tests' -Needs 'Glue build' -Body {
        # A build without the sanitizer would pass as well: it must load the sanitizer's runtime.
        $tests = 'build\windows-x64-glue\bin\Release\editor_tests.exe'
        $runtime = dumpbin /nologo /dependents $tests | Where-Object { $_ -match 'clang_rt\.asan' }
        if (-not $runtime) { throw "$tests is not built with AddressSanitizer." }
        Write-Host "editor_tests uses $($runtime.Trim())"
        Run ctest --preset windows-x64-glue
    }
}

if ($main) {
    if ($env:SKIP_PERFORMANCE -eq '1') {
        Write-Host "`nPerformance limits skipped (SKIP_PERFORMANCE=1)."
    } else {
        Stage -Name 'Performance limits' -Rerun 'scripts\performance.ps1' -Body { & (Join-Path $PSScriptRoot 'performance.ps1') }
    }
}

Report-Stages
