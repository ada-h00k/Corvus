; Corvus - Windows setup (Inno Setup 6.2 or newer)
;
; Build (done by .github/workflows/release.yml):
;   iscc /DAppVersion=0.2.0 /DStageDir=C:\path\to\staged\files packaging\windows\corvus.iss
;
; The setup is English only. It installs Corvus together with corvus-cli
; (https://github.com/ada-h00k/corvus-cli, in <app>\corvus-cli\), the Signal backend that
; replaces signal-cli: no Java and no download at install time. Corvus finds it by itself.
; signal-cli can still be used: select it in Corvus under Settings -> Advanced -> Choose ...

#ifndef AppVersion
  #define AppVersion "0.2.0"
#endif
#ifndef StageDir
  #define StageDir "..\..\build-windows\stage"
#endif
#define AppName "Corvus"
#define AppExe "corvus.exe"

[Setup]
AppId={{6F0B7D5E-3C1A-4B6E-9D52-7A1E4C8B2F90}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Corvus
AppPublisherURL=https://github.com/ada-h00k/Corvus
AppSupportURL=https://github.com/ada-h00k/Corvus/issues
AppUpdatesURL=https://github.com/ada-h00k/Corvus/releases
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
OutputDir=..\..\dist
OutputBaseFilename=corvus-windows-x64-setup
SetupIconFile=corvus.ico
UninstallDisplayIcon={app}\{#AppExe}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Per-user install by default (no admin rights needed); the user may choose "all users".
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[InstallDelete]
; Corvus was called Wren before. The AppId is unchanged, so this setup upgrades an existing
; installation in place (same folder); remove the files and shortcuts of the old name.
Type: files; Name: "{app}\wren.exe"
Type: files; Name: "{autoprograms}\Wren.lnk"
Type: files; Name: "{autodesktop}\Wren.lnk"

[UninstallDelete]
; Older setups downloaded signal-cli (and optionally a Java runtime) into this folder.
; Your Signal account data (corvus-cli: ~\.local\share\corvus-cli, signal-cli: ~\.local\share\signal-cli)
; and Corvus's chat history are NOT removed.
Type: filesandordirs; Name: "{app}\signal-cli"
