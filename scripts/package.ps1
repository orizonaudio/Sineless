# Builds the Release bundle with x64 and ARM64 binaries and zips it for GitHub Releases. It has
# no code signature.
#
# Output: build\release\Sineless-<version>-Windows.zip
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
. (Join-Path $PSScriptRoot 'windows-env.ps1')

$bundle = 'build\release\Sineless.vst3'
if (Test-Path $bundle) { Remove-Item -Recurse -Force $bundle }
foreach ($arch in 'x64', 'arm64') {
    Run cmake --preset "windows-$arch"
    Run cmake --build --preset "windows-$arch-release" -- /m /nologo /v:minimal
    # Each build has its own Contents\<arch>-win folder; robocopy merges them into one bundle.
    # Its exit codes below 8 mean success.
    robocopy "build\windows-$arch\VST3\Release\Sineless.vst3" $bundle /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Could not copy the $arch bundle." }
}
$global:LASTEXITCODE = 0

# Checks of C-2 on Windows: one binary per architecture, each of the right machine type, and
# no dependency on the Visual C++ runtime DLLs, which users may not have.
$machines = @{ 'x86_64-win' = 0x8664; 'arm64-win' = 0xAA64 }
foreach ($folder in $machines.Keys) {
    $binary = "$bundle\Contents\$folder\Sineless.vst3"
    if (-not (Test-Path $binary)) { throw "Missing $binary" }
    $bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $binary))
    $header = [BitConverter]::ToInt32($bytes, 0x3C)
    $machine = [BitConverter]::ToUInt16($bytes, $header + 4)
    if ($machine -ne $machines[$folder]) { throw ('{0} has machine type 0x{1:X4}' -f $binary, $machine) }
    $dependencies = dumpbin /nologo /dependents $binary | Where-Object { $_ -match '^\s+\S+\.dll$' } |
        ForEach-Object { $_.Trim() }
    $runtime = $dependencies | Where-Object { $_ -match '^(vcruntime|msvcp|ucrtbase|concrt|api-ms-win-crt)' }
    if ($runtime) { throw "$binary needs the C++ runtime DLLs: $($runtime -join ', ')" }
    Write-Host "$folder uses $($dependencies -join ', ')"
}

$version = (Select-String -Path CMakeLists.txt -Pattern 'project\(\S+ VERSION (\S+)').Matches[0].Groups[1].Value
$archive = "build\release\Sineless-$version-Windows.zip"
if (Test-Path $archive) { Remove-Item $archive }
# tar, not Compress-Archive: Windows PowerShell 5.1 writes backslashes into the entry names.
Run tar.exe -a -c -f $archive -C build\release Sineless.vst3
Write-Host ('Packaged {0} ({1:N1} MB)' -f $archive, ((Get-Item $archive).Length / 1MB))
