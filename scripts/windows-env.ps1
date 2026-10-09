# Dot-sourced by the Windows scripts: enters Visual Studio's developer environment, which
# provides CMake, MSBuild, the compilers and dumpbin, unless it is already active.

# ARM64 Windows runs x64 programs too; an x64 machine cannot run ARM64 ones.
$hostIsArm = $env:PROCESSOR_ARCHITECTURE -eq 'ARM64'

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio 2022 with the C++ tools is required.' }
    Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    $arch = if ($hostIsArm) { 'arm64' } else { 'amd64' }
    # -Arch/-HostArch lack arm64; the underlying VsDevCmd accepts it.
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=$arch -host_arch=$arch" | Out-Null
}

function Run {
    & $args[0] $args[1..($args.Count - 1)]
    if ($LASTEXITCODE -ne 0) { throw "Failed: $($args -join ' ')" }
}
