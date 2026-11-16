; Inno Setup script for the RedCard 2003 recompilation (Windows 10/11, 64-bit).
; Ported from NHL Hitz Pro's nhl_hitz_pro.iss. The optional hi-res texture pack is downloaded
; from the GitHub release (or taken from a texture.bin beside Setup), checked and extracted:
; texture_payload.iss pins it, texture_download_code.iss does it.
; Build the player exe and the import tool first (see scripts/build_installer.ps1), then:
;   "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" /DAppVersion=... /DBuildDir=... installer\redcard_2003.iss
; Output: installer\Output\RedCard2003-Setup-<version>.exe
; If the game is already installed (its game files are in place), Setup only updates the program files:
; it does not ask for the disc and does not copy the ~2.5 GB of game files again.
#ifndef AppVersion
#define AppVersion "1.0.0"
#endif
#ifndef OutSuffix
#define OutSuffix ""
#endif
#ifndef GameGuid
#define GameGuid "A7C3E91F-2B84-4D6A-9F15-8E0C4B72D3A1"
#endif
#ifndef GameShortcutName
#define GameShortcutName "RedCard 2003"
#endif
#ifndef BuildDir
#define BuildDir "..\build\Release"
#endif
; The build always outputs redcard_recomp.exe. A release installs it as "Redcard 2003.exe";
; the diagnostic (DebugBuild) package keeps the build name so its logs and docs match.
#ifdef DebugBuild
#define GameExe "redcard_recomp.exe"
#else
#define GameExe "Redcard 2003.exe"
#endif
#include "texture_payload.iss"

[Setup]
AppId={{{#GameGuid}}
AppName=RedCard 2003
AppVersion={#AppVersion}
AppPublisher=dalves408/redcard2003recomp
AppPublisherURL=https://github.com/dalves408/redcard2003recomp
DefaultDirName=C:\Games\RedCard 2003
DefaultGroupName=RedCard 2003
DisableProgramGroupPage=yes
; fresh install defaults to C:\Games\RedCard 2003; an existing install is found by AppId and its folder is pre-filled; the folder page is always shown so it can be changed
DisableDirPage=no
UsePreviousAppDir=yes
; a running game is closed so its exe can be replaced
CloseApplications=yes
CloseApplicationsFilter=Redcard 2003.exe,redcard_recomp.exe,config.exe
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
SetupIconFile=..\resources\redcard_2003.ico
; the picture at the top right of every wizard page is the same icon (the sizes are for different screen scalings)
WizardSmallImageFile=wizard_small_55.bmp,wizard_small_64.bmp,wizard_small_80.bmp,wizard_small_96.bmp,wizard_small_128.bmp
UninstallDisplayIcon={app}\redcard_2003.ico
OutputBaseFilename=RedCard2003-Setup-{#AppVersion}{#OutSuffix}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchiveExtraction=enhanced/nopassword

[Messages]
WelcomeLabel2=This installs the RedCard 2003 PC recompilation.%n%nThe game's own files are not included. You need your own copy of the Xbox disc: a disc image (.iso or .xiso) or a folder with the game's files. Setup will ask for it and copy the files it needs.

[Files]
Source: "{#BuildDir}\redcard_recomp.exe"; DestDir: "{app}"; DestName: "{#GameExe}"; Flags: ignoreversion
Source: "{#BuildDir}\config.exe"; DestDir: "{app}"; Flags: ignoreversion
; same mark as SetupIconFile / exe .rc — Start Menu + desktop shortcuts use IconFilename
Source: "..\resources\redcard_2003.ico"; DestDir: "{app}"; Flags: ignoreversion
; the import tool only runs during setup; it is not installed
Source: "{#BuildDir}\redcard_import.exe"; DestDir: "{tmp}"; Flags: ignoreversion deleteafterinstall
#ifdef DebugBuild
Source: "..\scripts\collect_debug.ps1"; DestDir: "{app}"; Flags: ignoreversion
#endif
; RecompLauncher reads this (its Developer tools can also add an installed copy as a build)
Source: "{#BuildDir}\recomplauncher.json"; DestDir: "{app}"; Flags: ignoreversion
; the hi-res texture download helper: extracted to {tmp} only when the texture task is ticked
Source: "{#BuildDir}\redcard_texture_download.exe"; Flags: dontcopy

[Icons]
#ifdef DebugBuild
Name: "{autoprograms}\RedCard 2003 - Debug"; Filename: "{app}\{#GameExe}"; WorkingDir: "{app}"; IconFilename: "{app}\redcard_2003.ico"
Name: "{autodesktop}\RedCard 2003 - Debug"; Filename: "{app}\{#GameExe}"; WorkingDir: "{app}"; IconFilename: "{app}\redcard_2003.ico"; Tasks: desktopicon
Name: "{autoprograms}\RedCard 2003 - Collect debug files"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\collect_debug.ps1"""; WorkingDir: "{app}"; IconFilename: "{app}\redcard_2003.ico"
Name: "{autoprograms}\{#GameShortcutName} Settings"; Filename: "{app}\config.exe"; WorkingDir: "{app}"; IconFilename: "{app}\redcard_2003.ico"
#else
Name: "{autoprograms}\{#GameShortcutName}"; Filename: "{app}\{#GameExe}"; WorkingDir: "{app}"; IconFilename: "{app}\redcard_2003.ico"
Name: "{autodesktop}\{#GameShortcutName}"; Filename: "{app}\{#GameExe}"; WorkingDir: "{app}"; IconFilename: "{app}\redcard_2003.ico"; Tasks: desktopicon
Name: "{autoprograms}\{#GameShortcutName} Settings"; Filename: "{app}\config.exe"; WorkingDir: "{app}"; IconFilename: "{app}\redcard_2003.ico"
#endif

[Tasks]
Name: "hires"; Description: "Download and install the 4x hi-res texture pack ({#TextureVersion}: 1.03 GB download, 1.9 GB on disk)"; Flags: unchecked; Check: TexturePackNeedsInstall
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

#ifndef DebugBuild
[InstallDelete]
; An update over an older release (or a debug package) leaves the old exe name behind.
Type: files; Name: "{app}\redcard_recomp.exe"
Type: files; Name: "{app}\redcard_recomp.pdb"
#endif

[UninstallDelete]
Type: filesandordirs; Name: "{app}\game_files"
Type: filesandordirs; Name: "{app}\texture_pack"
; written by the game while it runs (logs\ only with portable.txt)
Type: files; Name: "{app}\xbox_kernel.log"
Type: filesandordirs; Name: "{app}\logs"

[Code]
var
  SrcPage, UpdatePage: TWizardPage;
  SrcEdit: TNewEdit;
  UpdateOnly, UpdateMode: Boolean;
  OldDir, OldVer, NewVer: String;
  VerRel: Integer;          { -1: the installed version is older, 0: same, 1: newer than this Setup }
  RepairRadio, QuitRadio: TNewRadioButton;
  QuitNow: Boolean;

#include "texture_download_code.iss"

{ The game files are in place: default.xbe and a few of the largest files, so an install that was
  interrupted halfway is not mistaken for a finished one. }
function GameFilesPresent(Dir: String): Boolean;
begin
  Result := FileExists(Dir + '\game_files\default.xbe') and
            FileExists(Dir + '\game_files\SOUNDS\RCSPNSND.ZIP') and
            FileExists(Dir + '\game_files\MOVIES\intro_n.avi') and
            FileExists(Dir + '\game_files\TEAMS\NET\NET_TEAM.XRS');
end;

{ Where a previous Setup put the game, or '' }
function PreviousAppDir: String;
var
  Key, Dir: String;
begin
  Result := '';
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{{#GameGuid}}_is1';
  if RegQueryStringValue(HKCU, Key, 'Inno Setup: App Path', Dir) or
     RegQueryStringValue(HKLM, Key, 'Inno Setup: App Path', Dir) then
    Result := RemoveBackslashUnlessRoot(Dir);
end;

{ The version a previous Setup recorded, or '' }
function PreviousVersion: String;
var
  Key, V: String;
begin
  Result := '';
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{{#GameGuid}}_is1';
  if RegQueryStringValue(HKCU, Key, 'DisplayVersion', V) or
     RegQueryStringValue(HKLM, Key, 'DisplayVersion', V) then
    Result := V;
end;

{ The n-th dot-separated number of a version such as 1.0.2 (0 when missing) }
function VerPart(V: String; N: Integer): Integer;
var
  I, P: Integer;
begin
  for I := 1 to N do
  begin
    P := Pos('.', V);
    if P = 0 then begin V := ''; Break; end;
    V := Copy(V, P + 1, Length(V));
  end;
  P := Pos('.', V);
  if P > 0 then V := Copy(V, 1, P - 1);
  Result := StrToIntDef(V, 0);
end;

{ -1 if A is older than B, 0 if the same, 1 if newer }
function CompareVer(A, B: String): Integer;
var
  I: Integer;
begin
  Result := 0;
  for I := 0 to 3 do
    if VerPart(A, I) <> VerPart(B, I) then
    begin
      if VerPart(A, I) < VerPart(B, I) then Result := -1 else Result := 1;
      Exit;
    end;
end;

{ The page that says what was found and asks the player to go ahead }
var
  UpdY: Integer;

procedure AddLine(Text: String; Bold: Boolean);
var
  L: TNewStaticText;
begin
  L := TNewStaticText.Create(UpdatePage);
  L.Parent := UpdatePage.Surface;
  L.AutoSize := False;
  L.WordWrap := True;
  L.Left := 0;
  L.Top := UpdY;
  L.Width := UpdatePage.SurfaceWidth;
  if Bold then L.Font.Style := [fsBold];
  L.Caption := Text;
  L.Height := ScaleY(18) * (1 + Length(Text) div 70);
  UpdY := UpdY + L.Height + ScaleY(6);
end;

procedure RadioClick(Sender: TObject);
begin
  if QuitRadio.Checked then
    WizardForm.NextButton.Caption := 'Quit'
  else
    WizardForm.NextButton.Caption := 'Repair';
end;

procedure AddChoice(var R: TNewRadioButton; Text: String; Checked: Boolean);
begin
  R := TNewRadioButton.Create(UpdatePage);
  R.Parent := UpdatePage.Surface;
  R.Left := 0;
  R.Top := UpdY;
  R.Width := UpdatePage.SurfaceWidth;
  R.Height := ScaleY(20);
  R.Caption := Text;
  R.Checked := Checked;
  R.OnClick := @RadioClick;
  UpdY := UpdY + R.Height + ScaleY(6);
end;

procedure CreateUpdatePage;
begin
  if VerRel = 0 then
    UpdatePage := CreateCustomPage(wpWelcome, 'This version is already installed',
      'Setup found the same version of RedCard 2003 on this computer.')
  else
    UpdatePage := CreateCustomPage(wpWelcome, 'RedCard 2003 is already installed',
      'Setup found an existing installation of the game on this computer.');
  UpdY := 0;
  AddLine('Installed in:  ' + OldDir, False);
  if OldVer <> '' then
    AddLine('Installed version:  ' + OldVer, False)
  else
    AddLine('Installed version:  (not recorded)', False);
  AddLine('This Setup:  version ' + NewVer, True);
  UpdY := UpdY + ScaleY(10);
  if VerRel = 0 then
  begin
    AddLine('The installed version matches this Setup. You can repair the installation, which reinstalls the ' +
            'program files and shortcuts. Your game files, saves and settings are kept, and you do not need your disc.', False);
    UpdY := UpdY + ScaleY(10);
    AddChoice(RepairRadio, 'Repair the installation', True);
    AddChoice(QuitRadio, 'Quit (leave everything as it is)', False);
    Exit;
  end;
  if VerRel < 0 then
    AddLine('Setup will update the program files from version ' + OldVer + ' to version ' + NewVer +
            '. Your game files, saves and settings are kept, and you do not need your disc.', False)
  else
    AddLine('A NEWER version than this Setup is installed. Continuing will replace it with the older version ' +
            NewVer + '. Your game files, saves and settings are kept.', False);
  UpdY := UpdY + ScaleY(10);
  AddLine('Click Update to continue, or Cancel to leave the installed version as it is.', False);
end;

procedure BrowseFile(Sender: TObject);
var
  F: String;
begin
  F := SrcEdit.Text;
  if GetOpenFileName('Select the disc image', F, '', 'Xbox disc images (*.iso;*.xiso)|*.iso;*.xiso|All files (*.*)|*.*', 'iso') then
    SrcEdit.Text := F;
end;

procedure BrowseFolder(Sender: TObject);
var
  D: String;
begin
  D := SrcEdit.Text;
  if BrowseForFolder('Select the folder that holds the game''s files (the one with default.xbe)', D, False) then
    SrcEdit.Text := D;
end;

procedure InitializeWizard;
var
  L: TNewStaticText;
  B1, B2: TNewButton;
begin
  NewVer := '{#AppVersion}';
  OldDir := PreviousAppDir;
  UpdateMode := (OldDir <> '') and GameFilesPresent(OldDir);
  if UpdateMode then
  begin
    OldVer := PreviousVersion;
    if OldVer <> '' then VerRel := CompareVer(OldVer, NewVer) else VerRel := -1;
    WizardForm.WelcomeLabel2.Caption :=
      'RedCard 2003 is already installed on this computer.' + #13#10#13#10 +
      'Setup will show the installed version and this Setup''s version on the next page and ask you to confirm before it changes anything.';
    CreateUpdatePage;
  end;
  SrcPage := CreateCustomPage(wpSelectDir, 'Your copy of the game',
    'Select the disc image or folder that Setup should copy the game files from.');
  L := TNewStaticText.Create(SrcPage);
  L.Parent := SrcPage.Surface;
  L.Caption := 'Disc image (.iso, .xiso) or a folder containing default.xbe:';
  L.Left := 0; L.Top := 0;
  SrcEdit := TNewEdit.Create(SrcPage);
  SrcEdit.Parent := SrcPage.Surface;
  SrcEdit.Left := 0; SrcEdit.Top := L.Height + 8;
  SrcEdit.Width := SrcPage.SurfaceWidth;
  SrcEdit.Text := ExpandConstant('{param:SOURCE|}');   { setup /SOURCE=<image or folder> for silent installs }
  B1 := TNewButton.Create(SrcPage);
  B1.Parent := SrcPage.Surface;
  B1.Caption := 'Disc image...';
  B1.Left := 0; B1.Top := SrcEdit.Top + SrcEdit.Height + 8;
  B1.Width := ScaleX(110);
  B1.OnClick := @BrowseFile;
  B2 := TNewButton.Create(SrcPage);
  B2.Parent := SrcPage.Surface;
  B2.Caption := 'Folder...';
  B2.Left := B1.Left + B1.Width + 8; B2.Top := B1.Top;
  B2.Width := ScaleX(110);
  B2.OnClick := @BrowseFolder;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if UpdateMode and (CurPageID = UpdatePage.ID) then
  begin
    if VerRel = 0 then RadioClick(nil) else WizardForm.NextButton.Caption := 'Update';
  end
  else if UpdateMode and UpdateOnly and (CurPageID = wpFinished) then
  begin
    if VerRel = 0 then
      WizardForm.FinishedLabel.Caption := 'RedCard 2003 ' + NewVer + ' was repaired.' + #13#10#13#10 +
        'Your game files, saves and settings were kept.'
    else
      WizardForm.FinishedLabel.Caption := 'RedCard 2003 was updated to version ' + NewVer + '.' + #13#10#13#10 +
        'Your game files, saves and settings were kept.';
  end
  ;
  if (CurPageID = wpFinished) and WantTexturePack and TexturePackInstalled then
    WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10#13#10 +
      'Hi-res textures are installed and on. Turn them off or on with Hi-res textures in Settings or the in-game F1 menu.';
end;

{ The "Ready to install" page says what is about to happen. }
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
var
  S: String;
begin
  if UpdateMode and GameFilesPresent(WizardDirValue) then
  begin
    if VerRel = 0 then
      S := 'Repair RedCard 2003 ' + NewVer + ' in:'
    else
    begin
      S := 'Update RedCard 2003';
      if OldVer <> '' then S := S + ' from version ' + OldVer;
      S := S + ' to version ' + NewVer + ' in:';
    end;
    S := S + NewLine + Space + WizardDirValue + NewLine + NewLine +
         'Only the program files are replaced. Your game files, saves and settings are kept.';
    if MemoTasksInfo <> '' then S := S + NewLine + NewLine + MemoTasksInfo;
    Result := S;
  end else
  begin
    S := '';
    if MemoDirInfo <> '' then S := S + MemoDirInfo + NewLine + NewLine;
    if MemoTasksInfo <> '' then S := S + MemoTasksInfo + NewLine;
    Result := S;
  end;
end;

{ No disc is needed when the game files are already in the chosen folder. }
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = SrcPage.ID) and GameFilesPresent(WizardDirValue);
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  if QuitNow then Confirm := False;      { Quit on the matching-version page: no "Exit Setup?" question }
  TextureCancelButtonClick(Cancel, Confirm);   { during the texture download: stop it, keep Setup open }
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if UpdateMode and (VerRel = 0) and (CurPageID = UpdatePage.ID) and QuitRadio.Checked then
  begin
    QuitNow := True;
    WizardForm.Close;
    Result := False;
    Exit;
  end;
  if CurPageID = SrcPage.ID then
    if (Trim(SrcEdit.Text) = '') or
       not (FileExists(Trim(SrcEdit.Text)) or DirExists(Trim(SrcEdit.Text))) then
    begin
      SuppressibleMsgBox('Select an existing disc image or folder.', mbError, MB_OK, IDOK);
      Result := False;
    end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not GameFilesPresent(WizardDirValue) then begin
    if (Trim(SrcEdit.Text) = '') or
       not (FileExists(Trim(SrcEdit.Text)) or DirExists(Trim(SrcEdit.Text))) then begin
      Result := 'Select your RedCard 2003 Xbox USA disc image or extracted folder.';
      Exit;
    end;
    Result := PrepareTexturePayload(2600000000);   { leave room for the game files too }
  end else
    Result := PrepareTexturePayload(0);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
  Msg: String;
begin
  if CurStep = ssInstall then
    UpdateOnly := GameFilesPresent(ExpandConstant('{app}'));
  if (CurStep = ssPostInstall) and not UpdateOnly then
  begin
    { A fresh install starts fullscreen. An update or repair (game files already there) keeps
      the player's settings. Settings, F1 and Alt+Enter still switch it. }
    ForceDirectories(ExpandConstant('{localappdata}\RedCard2003'));
    if not SetIniString('display', 'fullscreen', '1', ExpandConstant('{localappdata}\RedCard2003\settings.ini')) then
      Log('Could not set fullscreen in settings.ini.');
    WizardForm.StatusLabel.Caption := 'Copying the game files from your disc (about 2.5 GB, a few minutes)...';
    WizardForm.FilenameLabel.Caption := '';
    if not Exec(ExpandConstant('{tmp}\redcard_import.exe'),
        '--prepare ' + AddQuotes(Trim(SrcEdit.Text)) + ' ' + AddQuotes(ExpandConstant('{app}\game_files')),
        '', SW_HIDE, ewWaitUntilTerminated, Code) then
      Code := 99;
    if Code <> 0 then
    begin
      case Code of
        2: Msg := 'The disc image or folder could not be found.';
        3: Msg := 'That file is not an Xbox disc image.';
        4: Msg := 'That is not the supported version of RedCard 2003 (the Xbox USA release).';
        5: Msg := 'Some game files are missing or damaged. The dump may be incomplete.';
        6: Msg := 'The game files could not be written to the install folder.';
      else
        Msg := 'The game files could not be copied.';
      end;
      SuppressibleMsgBox(Msg + #13#10#13#10 + 'RedCard 2003 was installed but cannot start yet. ' +
        'Run Setup again and select a correct disc image or folder.', mbError, MB_OK, IDOK);
    end;
  end;
  if CurStep = ssPostInstall then
    InstallTexturePack;
  if CurStep = ssDone then
    TextureFinishCacheChoice;
end;

procedure DeinitializeSetup;
begin
  TextureCleanupOnExit;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  { [UninstallDelete] removes texture_pack; remove its Programs and Features entry with it }
  if CurUninstallStep = usPostUninstall then
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{{#PackGuid}}_is1');
end;

function GetCustomSetupExitCode: Integer;
begin
  Result := 0;
  if not GameFilesPresent(ExpandConstant('{app}')) then
    Result := 1;
end;
