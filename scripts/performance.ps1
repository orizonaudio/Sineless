# The audio performance limit on Windows (x64, optimised benchmark build). Run by
# scripts\check.ps1 unless SKIP_PERFORMANCE=1, and by CI as a separate job.
#
# Environment:
#   RUN_BENCH 1 to run it here; it runs by default on GitHub's machines only, since its limit
#             means something on real x64 hardware alone (not under emulation)
#   PERF_SPEED report to measure without judging, as CI sets it: its shared machines are noisier
#             than the hardware the limit is set for (see scripts/performance.sh)
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
. (Join-Path $PSScriptRoot 'windows-env.ps1')

Write-Host "`n==> Audio performance (x64, optimised benchmark build)"
if ($env:GITHUB_ACTIONS -eq 'true' -or $env:RUN_BENCH -eq '1') {
    Run cmake --preset windows-x64-bench
    Run cmake --build --preset windows-x64-bench -- /m /nologo /v:minimal
    # The same limit as on macOS: eight instances within 5 % of each block's deadline at p99.9.
    $output = & build\windows-x64-bench\bin\Release\process_bench.exe --instances=8 --seconds=30 --max-share=0.05
    $result = $LASTEXITCODE
    $output | Select-Object -Last 1 | Write-Host
    # Over its budget it exits 1; anything else is a crash, never only reported.
    if ($result -ne 0 -and ($result -ne 1 -or $env:PERF_SPEED -ne 'report')) {
        throw "process_bench failed (exit code $result)."
    }
    # Reported, not judged: the step must not fail on the exit code left behind.
    $global:LASTEXITCODE = 0
} else {
    Write-Host 'Skipped: set RUN_BENCH=1 on real x64 hardware (it runs on GitHub by default).'
}
