#ifndef MyAppVersion
  #define MyAppVersion "0.3.0-beta.14"
#endif
#ifndef MyFileVersion
  #define MyFileVersion "0.3.0.14"
#endif
#ifndef PayloadRoot
  #define PayloadRoot "..\artifacts\payload"
#endif
#ifndef OutputDirectory
  #define OutputDirectory "..\artifacts"
#endif

; 简体中文语言文件随仓库提供（社区翻译，面向 Inno Setup 6.5+）。
; 若该文件缺失，下面的 #if 会让编译器跳过中文，安装程序仍能正常构建（仅英文界面）。
#define ChineseIslFile "languages\ChineseSimplified.isl"
#define HasChineseIsl FileExists(AddBackslash(SourcePath) + ChineseIslFile)

; AppPublisher / VersionInfoCompany stay language-neutral on purpose: these
; values also surface in an English install (Ready page, Add/Remove Programs,
; file properties), so the localization must not put Chinese text there. The
; credit for the translation lives in docs/NOTICE-ZH-CN.md, which the Chinese
; wizard shows through its own InfoAfterFile.
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
VersionInfoCopyright=Copyright (c) 2022 Matthieu Bucchianeri. Simplified Chinese localization by idanfang (unofficial).
VersionInfoDescription=OpenXR OBS Mirror Setup
VersionInfoProductName=OpenXR OBS Mirror

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
#if HasChineseIsl
; InfoBeforeFile / InfoAfterFile 是 [Languages] 支持的按语言覆盖参数：
; 中文安装向导显示中文安装说明，并在完成页显示「关于本汉化版」声明。
Name: "chinesesimplified"; MessagesFile: "languages\ChineseSimplified.isl"; InfoBeforeFile: "..\docs\INSTALL.zh-CN.md"; InfoAfterFile: "..\docs\NOTICE-ZH-CN.md"
#endif

[CustomMessages]
english.CreateDesktopShortcut=Create a desktop shortcut
english.AdditionalShortcuts=Additional shortcuts:
english.RegisteringLayer=Registering the OpenXR mirror layer...
english.OpenControlCenter=Open Control Center
#if HasChineseIsl
chinesesimplified.CreateDesktopShortcut=创建桌面快捷方式
chinesesimplified.AdditionalShortcuts=附加快捷方式：
chinesesimplified.RegisteringLayer=正在注册 OpenXR 镜像层……
chinesesimplified.OpenControlCenter=打开控制中心
#endif

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopShortcut}"; GroupDescription: "{cm:AdditionalShortcuts}"; Flags: unchecked

[Files]
Source: "{#PayloadRoot}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadRoot}\bin\x64\Release\OBS_Plugin\win-openxr.dll"; DestDir: "{commonappdata}\obs-studio\plugins\win-openxr\bin\64bit"; Flags: ignoreversion restartreplace
Source: "{#PayloadRoot}\bin\x64\Release\OBS_Plugin\openvr_api.dll"; DestDir: "{commonappdata}\obs-studio\plugins\win-openxr\bin\64bit"; Flags: ignoreversion restartreplace
Source: "{#PayloadRoot}\OBSPlugin\win-openxr\data\*"; DestDir: "{commonappdata}\obs-studio\plugins\win-openxr\data"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\OpenXR OBS Mirror"; Filename: "{app}\ControlCenter\OBSMirror.ControlCenter.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\OpenXR OBS Mirror"; Filename: "{app}\ControlCenter\OBSMirror.ControlCenter.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\scripts\Setup-OBS.ps1"" -AllowRunningOBS -SkipPluginInstall"; StatusMsg: "{cm:RegisteringLayer}"; Flags: runhidden waituntilterminated runasoriginaluser
; skipifsilent is required: the Control Center's in-app updater runs this
; installer with /VERYSILENT and reopens the app itself once setup exits, so
; letting Setup also launch it would start a second instance.
Filename: "{app}\ControlCenter\OBSMirror.ControlCenter.exe"; Description: "{cm:OpenControlCenter}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\scripts\Uninstall-OBSMirror.ps1"""; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterOpenXRLayer"
