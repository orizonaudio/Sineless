# Dot-sourced by the Windows scripts: enters Visual Studio's developer environment, which
# provides CMake, MSBuild, the compilers and dumpbin, unless it is already active. It is told by
# dumpbin, which only that environment provides: CMake may be on the PATH without it, as on
# GitHub's runners.

# ARM64 Windows runs x64 programs too; an x64 machine cannot run ARM64 ones.
$hostIsArm = $env:PROCESSOR_ARCHITECTURE -eq 'ARM64'

if (-not (Get-Command dumpbin -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio 2022 or later with the C++ tools is required.' }
    Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    $arch = if ($hostIsArm) { 'arm64' } else { 'amd64' }
    # -Arch/-HostArch lack arm64; the underlying VsDevCmd accepts it.
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=$arch -host_arch=$arch" | Out-Null
}

# Runs a program and throws if it fails. Its output is piped on: PowerShell does not wait for a
# program built for the Windows GUI subsystem, such as pluginval, unless its output is, and would
# read the exit code of the program before it. A "--" meant for the program is quoted ('--'):
# PowerShell takes a bare one as the end of Run's own parameters and drops it (about_Parsing).
function Run {
    & $args[0] $args[1..($args.Count - 1)] | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Failed: $($args -join ' ')" }
}
