# The audio performance limit on Windows (x64, optimised benchmark build). Run by
# scripts\check.ps1 unless SKIP_PERFORMANCE=1, and by CI as a separate job: shared CI machines are
# noisier than dedicated hardware, so there the limit is reported rather than enforced.
#
# Environment:
#   RUN_BENCH 1 to run it here; it runs by default on GitHub's machines only, since its limit
#             means something on real x64 hardware alone (not under emulation)
#   PERF_MAX_SHARE  Share of each block's deadline the audio thread may use (default 0.05); CI's
#             shared machines are noisier, so CI loosens it
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
. (Join-Path $PSScriptRoot 'windows-env.ps1')

Write-Host "`n==> Audio performance (x64, optimised benchmark build)"
if ($env:GITHUB_ACTIONS -eq 'true' -or $env:RUN_BENCH -eq '1') {
    Run cmake --preset windows-x64-bench
    Run cmake --build --preset windows-x64-bench '--' /m /nologo /v:minimal
    # The same limit as on macOS: eight instances within 5 % of each block's deadline at p99.9.
    $maxShare = if ($env:PERF_MAX_SHARE) { $env:PERF_MAX_SHARE } else { '0.05' }
    $output = & build\windows-x64-bench\bin\Release\process_bench.exe --instances=8 --seconds=30 --max-share=$maxShare
    $result = $LASTEXITCODE
    $output | Select-Object -Last 1 | Write-Host
    if ($result -ne 0) { throw 'process_bench is over its budget.' }
} else {
    Write-Host 'Skipped: set RUN_BENCH=1 on real x64 hardware (it runs on GitHub by default).'
}
