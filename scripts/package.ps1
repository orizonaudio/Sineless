# Builds the Release bundle, zips it and wraps it in an installer for GitHub Releases. Neither has
# a code signature. Only x64 is built for now; adding 'arm64' to $architectures (and an ARM64
# machine type below) brings ARM64 back, built on an ARM64 machine.
#
# Output: build\release\Sineless-<version>-Windows.zip and
# build\release\Sineless-<version>-Windows-Setup.exe
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
. (Join-Path $PSScriptRoot 'windows-env.ps1')

$bundle = 'build\release\Sineless.vst3'
if (Test-Path $bundle) { Remove-Item -Recurse -Force $bundle }
$architectures = @('x64')
foreach ($arch in $architectures) {
    Run cmake --preset "windows-$arch"
    Run cmake --build --preset "windows-$arch-release" '--' /m /nologo /v:minimal
    # Each build has its own Contents\<arch>-win folder; robocopy merges them into one bundle.
    # Its exit codes below 8 mean success.
    robocopy "build\windows-$arch\VST3\Release\Sineless.vst3" $bundle /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Could not copy the $arch bundle." }
}
$global:LASTEXITCODE = 0

# Checks of C-2 on Windows: one binary per architecture built, each of the right machine type, and
# no dependency on the Visual C++ runtime DLLs, which users may not have.
$machines = @{ 'x86_64-win' = 0x8664 }
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

# The installer (scripts\installer.iss), from the same checked bundle. Inno Setup's ISCC is on
# GitHub's Windows runners, where a release needs it; a local run without it stops after the zip,
# so the bundle and zip can still be built (install it with `winget install JRSoftware.InnoSetup`).
$iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
if (-not $iscc) { $iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" }
if (-not (Test-Path $iscc)) {
    if ($env:CI) { throw 'Inno Setup 6 is required to build the installer.' }
    Write-Warning 'Inno Setup 6 is not installed, so no installer was built: winget install JRSoftware.InnoSetup'
    return
}
$setup = "build\release\Sineless-$version-Windows-Setup.exe"
if (Test-Path $setup) { Remove-Item $setup }
Run $iscc /Q "/DVersion=$version" scripts\installer.iss
if (-not (Test-Path $setup)) { throw "ISCC did not write $setup" }

# With INSTALL_CHECK=1 (the release workflow, on a runner thrown away afterwards, as an
# administrator): installed silently, first where no VST3 folder exists yet and then over an older
# copy holding a file the new one lacks, the installed bundle must equal the built one;
# uninstalled, nothing of it may be left.
if ($env:INSTALL_CHECK -eq '1') {
    $vst3 = "$env:CommonProgramFiles\VST3"
    $installed = "$vst3\Sineless.vst3"
    $relative = { param($root) Get-ChildItem -Recurse -File $root |
        ForEach-Object { $_.FullName.Substring((Resolve-Path $root).Path.Length) } | Sort-Object }
    $checkInstalled = {
        $difference = Compare-Object (& $relative $bundle) (& $relative $installed)
        if ($difference) { throw "The installed bundle differs from the built one: $($difference | Out-String)" }
        foreach ($file in (& $relative $bundle)) {
            if ((Get-FileHash "$bundle$file").Hash -ne (Get-FileHash "$installed$file").Hash) {
                throw "Installed $file differs from the built one."
            }
        }
    }
    $install = {
        $process = Start-Process $setup -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
        if ($process.ExitCode -ne 0) { throw "The installer failed with exit code $($process.ExitCode)." }
    }
    # A first install, as on a machine with no VST3 plug-ins yet. On a CI runner, a VST3 folder it
    # has is moved aside meanwhile; elsewhere the check needs there to be none, since moving the
    # machine's plug-ins away is not this script's to do.
    $aside = "$env:CommonProgramFiles\VST3-before"
    if ((Test-Path $vst3) -and $env:CI) { Move-Item $vst3 $aside }
    if (-not (Test-Path $vst3)) {
        & $install
        & $checkInstalled
        Write-Host "Installed and checked $installed where no VST3 folder existed"
    } else {
        Write-Host "Skipped the first install: $vst3 exists"
    }
    if (Test-Path $aside) {
        Remove-Item -Recurse -Force $vst3
        Move-Item $aside $vst3
    }

    if (Test-Path $installed) { Remove-Item -Recurse -Force $installed }
    robocopy $bundle $installed /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw 'Could not place the older copy.' }
    $global:LASTEXITCODE = 0
    New-Item -ItemType File "$installed\Contents\Resources\left-by-an-older-version" -Force | Out-Null
    # Its binary differs from the new one, as an older release's would, so the check proves the
    # upgrade replaces it.
    [IO.File]::AppendAllText((Resolve-Path "$installed\Contents\x86_64-win\Sineless.vst3").Path, 'older')

    & $install
    & $checkInstalled

    # Uninstalled as users are told to: through its entry in Settings > Apps, the uninstall key
    # Inno Setup writes for the installer's AppId.
    $entry = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{6F1D5C2E-8B4A-4E7D-9C3F-2A5B7E9D1C40}_is1'
    if (-not (Test-Path $entry)) { throw 'Sineless is not listed in Settings > Apps.' }
    $listed = Get-ItemProperty $entry
    if ($listed.DisplayName -ne 'Sineless') { throw "Settings > Apps lists it as '$($listed.DisplayName)'." }
    $uninstaller = $listed.UninstallString.Trim('"')
    if (-not (Test-Path $uninstaller)) { throw "The listed uninstaller $uninstaller does not exist." }
    $process = Start-Process $uninstaller -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "The uninstaller failed with exit code $($process.ExitCode)." }
    # The uninstaller removes its own files from a copy of itself, which may still be finishing.
    for ($i = 0; $i -lt 20 -and (Test-Path "$env:ProgramFiles\Orizon Audio"); $i++) { Start-Sleep -Milliseconds 500 }
    foreach ($left in @($installed, "$env:ProgramFiles\Orizon Audio", $entry)) {
        if (Test-Path $left) { throw "The uninstaller left $left" }
    }
    Write-Host "Installed over an older copy, checked and uninstalled $installed"
}
Write-Host ('Packaged {0} ({1:N1} MB)' -f $setup, ((Get-Item $setup).Length / 1MB))
