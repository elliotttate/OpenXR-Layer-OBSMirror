#ifndef MyAppVersion
  #define MyAppVersion "0.3.0-beta.3"
#endif
#ifndef MyFileVersion
  #define MyFileVersion "0.3.0.3"
#endif
#ifndef PayloadRoot
  #define PayloadRoot "..\artifacts\payload"
#endif
#ifndef OutputDirectory
  #define OutputDirectory "..\artifacts"
#endif

[Setup]
AppId={{8B49FA68-2786-4DCB-9A42-AC20AEF8208C}
AppName=OpenXR OBS Mirror
AppVersion={#MyAppVersion}
AppVerName=OpenXR OBS Mirror {#MyAppVersion}
AppPublisher=Elliott Tate
AppPublisherURL=https://github.com/elliotttate/OpenXR-Layer-OBSMirror
AppSupportURL=https://github.com/elliotttate/OpenXR-Layer-OBSMirror/issues
AppUpdatesURL=https://github.com/elliotttate/OpenXR-Layer-OBSMirror/releases
DefaultDirName={autopf}\OpenXR OBS Mirror
DefaultGroupName=OpenXR OBS Mirror
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
InfoBeforeFile=..\docs\INSTALL.md
OutputDir={#OutputDirectory}
OutputBaseFilename=OpenXR-OBSMirror-{#MyAppVersion}-Setup
SetupIconFile=..\ControlCenter\Assets\OBSMirror.ControlCenter.ico
UninstallDisplayIcon={app}\ControlCenter\OBSMirror.ControlCenter.exe
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#MyFileVersion}
VersionInfoTextVersion={#MyAppVersion}
VersionInfoCompany=Elliott Tate
VersionInfoDescription=OpenXR OBS Mirror Setup
VersionInfoProductName=OpenXR OBS Mirror

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "{#PayloadRoot}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadRoot}\bin\x64\Release\OBS_Plugin\win-openxr.dll"; DestDir: "{commonappdata}\obs-studio\plugins\win-openxr\bin\64bit"; Flags: ignoreversion restartreplace
Source: "{#PayloadRoot}\OBSPlugin\win-openxr\data\*"; DestDir: "{commonappdata}\obs-studio\plugins\win-openxr\data"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\OpenXR OBS Mirror"; Filename: "{app}\ControlCenter\OBSMirror.ControlCenter.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\OpenXR OBS Mirror"; Filename: "{app}\ControlCenter\OBSMirror.ControlCenter.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\scripts\Setup-OBS.ps1"" -AllowRunningOBS -SkipPluginInstall"; StatusMsg: "Registering the OpenXR mirror layer..."; Flags: runhidden waituntilterminated runasoriginaluser
Filename: "{app}\ControlCenter\OBSMirror.ControlCenter.exe"; Description: "Open Control Center"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\scripts\Uninstall-OBSMirror.ps1"""; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterOpenXRLayer"
