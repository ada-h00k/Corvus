; Corvus - Windows setup (Inno Setup 6.2 or newer)
;
; Build (done by .github/workflows/release.yml):
;   iscc /DAppVersion=0.2.0 /DStageDir=C:\path\to\staged\files packaging\windows\corvus.iss
;
; The setup is English only. Besides installing Corvus it can optionally download
;   * signal-cli  - latest release from https://github.com/AsamK/signal-cli (SHA-256 checked), and
;   * a private Java runtime (Eclipse Temurin JRE) if no suitable Java is installed.
; Both are downloaded at install time and are NOT part of the setup file.
;
; Silent installs:  corvus-windows-x64-setup.exe /VERYSILENT /InstallSignalCli=1 [/InstallJava=1]

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
Source: "fetch-release-info.ps1"; Flags: dontcopy

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
; signal-cli (and the optional Java runtime) are created by the setup at run time.
; Your Signal account data (~\.local\share\signal-cli) and Corvus's chat history are NOT removed.
Type: filesandordirs; Name: "{app}\signal-cli"

[Code]
const
  // signal-cli needs at least this Java version (see its README). Raise it if that changes.
  MinJavaMajor = 25;

var
  SignalPage: TInputOptionWizardPage;
  DownloadPage: TDownloadWizardPage;
  JavaMajor: Integer;
  JavaOk: Boolean;
  SignalCliOnPath: Boolean;
  SignalCliArchive: String;   // file name inside {tmp}; '' = nothing downloaded
  JavaArchive: String;

function SetEnvironmentVariable(lpName, lpValue: String): Integer;
  external 'SetEnvironmentVariableW@kernel32.dll stdcall';

{ ---------- helpers ---------- }

function OnDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  if Progress = ProgressMax then
    Log(Format('Successfully downloaded %s to %s', [Url, ExpandConstant('{tmp}')]));
  Result := True;
end;

function CmdParam(const Name: String; Default: Boolean): Boolean;
var
  I: Integer;
  Prefix, V: String;
begin
  Result := Default;
  Prefix := '/' + Lowercase(Name) + '=';
  for I := 1 to ParamCount do
  begin
    V := Lowercase(ParamStr(I));
    if Pos(Prefix, V) = 1 then
    begin
      Delete(V, 1, Length(Prefix));
      Result := (V = '1') or (V = 'true') or (V = 'yes');
      Exit;
    end;
  end;
end;

function KeyValue(const Lines: TArrayOfString; const Key: String): String;
var
  I, P: Integer;
begin
  Result := '';
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    P := Pos('=', Lines[I]);
    if (P > 0) and (CompareText(Copy(Lines[I], 1, P - 1), Key) = 0) then
    begin
      Result := Trim(Copy(Lines[I], P + 1, Length(Lines[I])));
      Exit;
    end;
  end;
end;

{ Major version of the Java that signal-cli.bat would use (JAVA_HOME first, then PATH); 0 = none. }
function DetectJavaMajor: Integer;
var
  OutFile, JavaCmd, S: String;
  Code, P, N: Integer;
  Raw: AnsiString;
begin
  Result := 0;
  OutFile := ExpandConstant('{tmp}\java-version.txt');
  DeleteFile(OutFile);
  if GetEnv('JAVA_HOME') <> '' then
    JavaCmd := '"%JAVA_HOME%\bin\java.exe"'
  else
    JavaCmd := 'java';
  if not Exec(ExpandConstant('{cmd}'), '/S /C "' + JavaCmd + ' -version > "' + OutFile + '" 2>&1"',
              '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Exit;
  if not LoadStringFromFile(OutFile, Raw) then Exit;
  S := String(Raw);
  P := Pos('version "', S);
  if P = 0 then Exit;
  Delete(S, 1, P + 8);
  N := 0;
  P := 1;
  while (P <= Length(S)) and (S[P] >= '0') and (S[P] <= '9') do
  begin
    N := N * 10 + (Ord(S[P]) - Ord('0'));
    P := P + 1;
  end;
  Result := N;   // "1.8.0_x" gives 1, which is correctly treated as too old
end;

{ Unpacks .tar.gz / .zip with tar.exe (included in Windows 10 1803 and later). }
function Untar(const Archive, DestDir: String): Boolean;
var
  Code: Integer;
begin
  ForceDirectories(DestDir);
  Result := Exec(ExpandConstant('{sys}\tar.exe'),
                 '-xf "' + Archive + '" -C "' + DestDir + '" --strip-components=1',
                 '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
  if not Result then
    Log(Format('tar.exe failed for %s (exit code %d)', [Archive, Code]));
end;

{ ---------- wizard ---------- }

procedure InitializeWizard;
var
  Note: TNewStaticText;
begin
  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing),
                                     SetupMessage(msgPreparingDesc), @OnDownloadProgress);

  JavaMajor := DetectJavaMajor;
  JavaOk := JavaMajor >= MinJavaMajor;
  SignalCliOnPath := FileSearch('signal-cli.bat', GetEnv('PATH')) <> '';

  SignalPage := CreateInputOptionPage(wpSelectTasks,
    'signal-cli',
    'Corvus needs signal-cli to talk to Signal.',
    'Corvus is a graphical front end for signal-cli, which is a separate program. ' +
    'Setup can download it for you from the official GitHub releases page.',
    False, False);

  SignalPage.Add('Download and install the latest signal-cli from github.com/AsamK/signal-cli (about 120 MB)');
  SignalPage.Add('Download and install a private Java ' + IntToStr(MinJavaMajor) +
                 ' runtime (Eclipse Temurin JRE, about 50 MB)');

  if SignalCliOnPath then
  begin
    SignalPage.CheckListBox.ItemCaption[0] :=
      'Download and install signal-cli again (an installation was already found on your PATH)';
    SignalPage.Values[0] := False;
  end
  else
    SignalPage.Values[0] := True;

  if JavaOk then
  begin
    SignalPage.CheckListBox.ItemCaption[1] :=
      'Java ' + IntToStr(JavaMajor) + ' was found on this computer - no Java download needed';
    SignalPage.CheckListBox.ItemEnabled[1] := False;
    SignalPage.Values[1] := False;
  end
  else
    SignalPage.Values[1] := True;

  SignalPage.CheckListBox.Height := ScaleY(80);

  Note := TNewStaticText.Create(SignalPage);
  Note.Parent := SignalPage.Surface;
  Note.Left := 0;
  Note.Top := SignalPage.CheckListBox.Top + SignalPage.CheckListBox.Height + ScaleY(12);
  Note.Width := SignalPage.SurfaceWidth;
  Note.AutoSize := False;
  Note.WordWrap := True;
  Note.Height := ScaleY(90);
  Note.Caption :=
    'signal-cli is free software (GPL-3.0) by AsamK and is not part of Corvus. It is downloaded directly from ' +
    'github.com and its SHA-256 checksum is verified. It is installed into the Corvus folder and is removed ' +
    'again when you uninstall Corvus (your Signal account data is kept).' + #13#10#13#10 +
    'signal-cli requires Java ' + IntToStr(MinJavaMajor) + ' or newer. If you skip these downloads you can ' +
    'install both yourself later and select signal-cli in Corvus under Settings.';
end;

{ ---------- download (runs before the files are copied; also in silent mode) ---------- }

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  WantSignalCli, WantJava: Boolean;
  Script, InfoFile, Params, Err: String;
  Info: TArrayOfString;
  Code: Integer;
  Url, Sha: String;
begin
  Result := '';
  SignalCliArchive := '';
  JavaArchive := '';

  if WizardSilent then
  begin
    WantSignalCli := CmdParam('InstallSignalCli', False);
    WantJava := CmdParam('InstallJava', False) and not JavaOk;
  end
  else
  begin
    WantSignalCli := SignalPage.Values[0];
    WantJava := SignalPage.Values[1] and not JavaOk;
  end;

  if not (WantSignalCli or WantJava) then Exit;

  DownloadPage.Clear;
  DownloadPage.SetText('Looking up the latest releases ...', '');
  DownloadPage.Show;
  try
    try
      ExtractTemporaryFile('fetch-release-info.ps1');
      Script := ExpandConstant('{tmp}\fetch-release-info.ps1');
      InfoFile := ExpandConstant('{tmp}\release-info.txt');
      DeleteFile(InfoFile);

      Params := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + Script + '"' +
                ' -OutFile "' + InfoFile + '"' +
                ' -WantSignalCli ' + IntToStr(Ord(WantSignalCli)) +
                ' -WantJava ' + IntToStr(Ord(WantJava)) +
                ' -JavaMajor ' + IntToStr(MinJavaMajor);

      if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, Code) then
        RaiseException('PowerShell could not be started.');
      if not LoadStringsFromFile(InfoFile, Info) then
        RaiseException('Could not read the release information.');
      Err := KeyValue(Info, 'error');
      if (Code <> 0) or (Err <> '') then
        RaiseException('Could not look up the download locations: ' + Err);

      DownloadPage.Clear;
      if WantSignalCli then
      begin
        Url := KeyValue(Info, 'signalcli_url');
        Sha := KeyValue(Info, 'signalcli_sha256');
        if Url = '' then RaiseException('No signal-cli download address received.');
        DownloadPage.Add(Url, 'signal-cli.tar.gz', Sha);
        Log('signal-cli ' + KeyValue(Info, 'signalcli_tag') + ' from ' + Url + ' (sha256 ' + Sha + ')');
      end;
      if WantJava then
      begin
        Url := KeyValue(Info, 'java_url');
        Sha := KeyValue(Info, 'java_sha256');
        if Url = '' then RaiseException('No Java download address received.');
        DownloadPage.Add(Url, 'java-jre.zip', Sha);
        Log('Java from ' + Url + ' (sha256 ' + Sha + ')');
      end;

      DownloadPage.Download;

      if WantSignalCli then SignalCliArchive := ExpandConstant('{tmp}\signal-cli.tar.gz');
      if WantJava then JavaArchive := ExpandConstant('{tmp}\java-jre.zip');
    except
      if DownloadPage.AbortedByUser then
        Result := 'The download was cancelled.'
      else if SuppressibleMsgBox(
          'The download failed:' + #13#10#13#10 + GetExceptionMessage + #13#10#13#10 +
          'Do you want to continue and install Corvus without signal-cli' +
          ' (you can install it later from https://github.com/AsamK/signal-cli/releases)?',
          mbError, MB_YESNO, IDYES) = IDNO then
        Result := 'Setup was stopped because the download failed.';
    end;
  finally
    DownloadPage.Hide;
  end;
end;

{ ---------- unpack + check (after Corvus's files were copied) ---------- }

procedure CheckSignalCli(const SignalDir: String);
var
  Bat, OutFile: String;
  Code: Integer;
  Raw: AnsiString;
  Msg: String;
begin
  Bat := SignalDir + '\bin\signal-cli.bat';
  if not FileExists(Bat) then
  begin
    SuppressibleMsgBox('signal-cli was downloaded but ' + Bat + ' was not found after unpacking.',
                       mbError, MB_OK, IDOK);
    Exit;
  end;

  if FileExists(SignalDir + '\jre\bin\java.exe') then
    SetEnvironmentVariable('JAVA_HOME', SignalDir + '\jre');

  OutFile := ExpandConstant('{tmp}\signal-cli-version.txt');
  DeleteFile(OutFile);
  Exec(ExpandConstant('{cmd}'), '/S /C ""' + Bat + '" --version > "' + OutFile + '" 2>&1"',
       '', SW_HIDE, ewWaitUntilTerminated, Code);
  Raw := '';
  LoadStringFromFile(OutFile, Raw);
  Msg := Trim(String(Raw));
  Log('signal-cli --version -> exit code ' + IntToStr(Code) + ': ' + Msg);

  if Code <> 0 then
    SuppressibleMsgBox(
      'signal-cli was installed but could not be started.' + #13#10#13#10 +
      'It needs Java ' + IntToStr(MinJavaMajor) + ' or newer. You can get one from https://adoptium.net/ ' +
      'and then restart Corvus.' + #13#10#13#10 + Msg,
      mbInformation, MB_OK, IDOK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  SignalDir: String;
begin
  if CurStep <> ssPostInstall then Exit;
  if (SignalCliArchive = '') and (JavaArchive = '') then Exit;

  SignalDir := ExpandConstant('{app}\signal-cli');
  ForceDirectories(SignalDir);

  if JavaArchive <> '' then
  begin
    WizardForm.StatusLabel.Caption := 'Installing Java runtime ...';
    DelTree(SignalDir + '\jre', True, True, True);
    if not Untar(JavaArchive, SignalDir + '\jre') then
      SuppressibleMsgBox('The Java runtime could not be unpacked (tar.exe is part of Windows 10 1803 and later).',
                         mbError, MB_OK, IDOK);
  end;

  if SignalCliArchive <> '' then
  begin
    WizardForm.StatusLabel.Caption := 'Installing signal-cli ...';
    DelTree(SignalDir + '\bin', True, True, True);
    DelTree(SignalDir + '\lib', True, True, True);
    if Untar(SignalCliArchive, SignalDir) then
    begin
      WizardForm.StatusLabel.Caption := 'Checking signal-cli ...';
      CheckSignalCli(SignalDir);
    end
    else
      SuppressibleMsgBox('signal-cli could not be unpacked (tar.exe is part of Windows 10 1803 and later).',
                         mbError, MB_OK, IDOK);
  end;
end;
