; The Windows installer, compiled by scripts\package.ps1 with Inno Setup's ISCC from the bundle it
; built and checked: ISCC /DVersion=<x.y.z> scripts\installer.iss.
;
; It installs the bundle for all users in Common Files\VST3, the folder every host scans, which
; needs an administrator's permission. The VST3 per-user folder (%LOCALAPPDATA%\Programs\Common\VST3)
; is not offered: few hosts are known to scan it. The uninstaller lives outside the bundle, so that
; hosts scanning the VST3 folder never see it, and is listed in Settings > Apps.

#ifndef Version
  #error Pass the version: ISCC /DVersion=<x.y.z> scripts\installer.iss
#endif

[Setup]
; Fixed for every version: it is how Windows knows a newer installer upgrades this one. scripts\package.ps1 finds the Settings > Apps entry by it.
AppId={{6F1D5C2E-8B4A-4E7D-9C3F-2A5B7E9D1C40}
AppName=Sineless
AppVersion={#Version}
AppVerName=Sineless {#Version}
AppPublisher=Orizon Audio
AppPublisherURL=https://github.com/orizonaudio/Sineless
AppSupportURL=https://github.com/orizonaudio/Sineless/issues
DefaultDirName={commoncf64}\VST3\Sineless.vst3
DisableDirPage=yes
DisableProgramGroupPage=yes
UninstallFilesDir={commonpf64}\Orizon Audio\Sineless
UninstallDisplayName=Sineless
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
OutputDir=..\build\release
OutputBaseFilename=Sineless-{#Version}-Windows-Setup

[Messages]
WelcomeLabel2=This installs Sineless {#Version}, an open-source VST3 oscilloscope by Orizon Audio, in the VST3 folder every host scans:%n%nC:\Program Files\Common Files\VST3%n%nClose your host first. Afterwards, rescan plugins in it if Sineless does not appear.

[InstallDelete]
; A clean upgrade: nothing of an older bundle is left beside the new one.
Type: filesandordirs; Name: "{app}"

[Files]
Source: "..\build\release\Sineless.vst3\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
Type: dirifempty; Name: "{app}"
Type: dirifempty; Name: "{commonpf64}\Orizon Audio"
