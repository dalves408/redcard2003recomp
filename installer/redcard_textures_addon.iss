; RedCard 2003 Hi-Res Textures: standalone add-on Setup for an installed game.
; Uses texture.bin beside this exe when it is there; otherwise downloads it from the GitHub
; release (resumable). Either way it is used only when its size and SHA-256 match
; texture_payload.iss, then extracted, checked against manifest.txt and put in <game>\texture_pack.
; Built by scripts/build_installer.ps1 -> installer\Output\RedCard2003-HiRes-Textures-<version>-Setup.exe
#define TextureAlways
#include "texture_payload.iss"
#ifndef BuildDir
#define BuildDir "..\build\Release"
#endif
#ifndef GameGuid
#define GameGuid "A7C3E91F-2B84-4D6A-9F15-8E0C4B72D3A1"
#endif
#ifndef OutSuffix
#define OutSuffix ""
#endif

[Setup]
AppId={{{#PackGuid}}
AppName=RedCard 2003 Hi-Res Textures
AppVersion={#TextureVersion}
AppPublisher=dalves408/redcard2003recomp
AppPublisherURL=https://github.com/dalves408/redcard2003recomp
DefaultDirName={code:GameDir}
UsePreviousAppDir=no
AppendDefaultDirName=no
DisableDirPage=no
DisableProgramGroupPage=yes
DirExistsWarning=no
; the game's own uninstaller removes texture_pack; this Setup registers the pack itself
Uninstallable=no
CreateUninstallRegKey=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
CloseApplications=no
SetupIconFile=..\resources\redcard_2003.ico
WizardSmallImageFile=wizard_small_55.bmp,wizard_small_64.bmp,wizard_small_80.bmp,wizard_small_96.bmp,wizard_small_128.bmp
OutputBaseFilename=RedCard2003-HiRes-Textures-{#TextureVersion}-Setup{#OutSuffix}
ArchiveExtraction=enhanced/nopassword
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Messages]
WelcomeLabel2=This adds the 4x hi-res textures to your installed RedCard 2003.%n%nIf texture.bin is next to this Setup it is used. Otherwise Setup downloads it (1.03 GB); a stopped download resumes the next time you run Setup.
SelectDirLabel3=Choose the folder where RedCard 2003 is installed.
ButtonInstall=&Install
ReadyLabel1=Ready to install the hi-res texture pack.
ReadyLabel2a=Click Install to continue, or Back to choose another game folder.
FinishedHeadingLabel=Hi-res textures installed
FinishedLabel=The hi-res textures are on. Turn them off or on with Hi-res textures in the game's Settings (config.exe) or the in-game F1 menu.

[Files]
Source: "{#BuildDir}\redcard_texture_download.exe"; Flags: dontcopy

[Code]
#include "texture_download_code.iss"
const
  GameKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{{#GameGuid}}_is1';

function GameExists(Dir: String): Boolean;
begin
  Result := (FileExists(Dir + '\Redcard 2003.exe') or FileExists(Dir + '\redcard_recomp.exe')) and
            FileExists(Dir + '\game_files\default.xbe');
end;

function FindGameDir: String;
var Dir: String;
begin
  Result := '';
  if RegQueryStringValue(HKCU, GameKey, 'Inno Setup: App Path', Dir) and
     GameExists(RemoveBackslashUnlessRoot(Dir)) then
    Result := RemoveBackslashUnlessRoot(Dir)
  else if RegQueryStringValue(HKLM, GameKey, 'Inno Setup: App Path', Dir) and
     GameExists(RemoveBackslashUnlessRoot(Dir)) then
    Result := RemoveBackslashUnlessRoot(Dir);
end;

function GameDir(Param: String): String;
begin
  Result := FindGameDir;
  if Result = '' then Result := 'C:\Games\RedCard 2003';
end;

function CheckGame(Dir: String): String;
begin
  Result := '';
  if not GameExists(Dir) then
    Result := 'Choose the folder where RedCard 2003 is installed (the one with Redcard 2003.exe and game_files).';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var Error: String;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and not WizardSilent then begin
    Error := CheckGame(RemoveBackslashUnlessRoot(WizardDirValue));
    if Error <> '' then begin
      MsgBox(Error, mbError, MB_OK);
      Result := False;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  { a silent install fails cleanly instead of waiting at a folder-page message }
  Result := CheckGame(RemoveBackslashUnlessRoot(ExpandConstant('{app}')));
  if Result = '' then Result := PrepareTexturePayload(1048576);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpReady then begin
    if TexturePackNeedsInstall then begin
      WizardForm.NextButton.Caption := SetupMessage(msgButtonInstall);
      WizardForm.PageNameLabel.Caption := SetupMessage(msgWizardReady);
      WizardForm.PageDescriptionLabel.Caption := SetupMessage(msgReadyLabel1);
      WizardForm.ReadyLabel.Caption := SetupMessage(msgReadyLabel2a);
    end else begin
      WizardForm.NextButton.Caption := '&Continue';
      WizardForm.PageNameLabel.Caption := 'Texture pack already installed';
      WizardForm.PageDescriptionLabel.Caption := 'This version of the texture pack is already installed.';
      WizardForm.ReadyLabel.Caption := 'Click Continue to finish. Nothing needs to be downloaded or installed.';
    end;
  end;
  if (CurPageID = wpFinished) and TexturePackAlreadyCurrent then begin
    WizardForm.FinishedHeadingLabel.Caption := 'Texture pack already installed';
    WizardForm.FinishedLabel.Caption := 'This version of the 4x texture pack is already installed. Nothing was downloaded.';
  end else if (CurPageID = wpFinished) and not TexturePackInstalled then begin
    WizardForm.FinishedHeadingLabel.Caption := 'Hi-res textures not installed';
    WizardForm.FinishedLabel.Caption := 'The texture pack could not be installed. Run this Setup again to retry; ' +
      'a downloaded archive is kept, so it is not downloaded again.';
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo;
  if not TexturePackNeedsInstall then
    Result := Result + NewLine + NewLine + 'This version of the texture pack is already installed.'
  else if FileExists(TextureLocalPart) then
    Result := Result + NewLine + NewLine + 'Texture pack: texture.bin next to this Setup (checked before use).'
  else
    Result := Result + NewLine + NewLine + 'Texture pack: downloaded from GitHub (1.03 GB), then checked.';
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  TextureCancelButtonClick(Cancel, Confirm);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then InstallTexturePack;
  if CurStep = ssDone then TextureFinishCacheChoice;
end;

procedure DeinitializeSetup;
begin
  TextureCleanupOnExit;
end;

function GetCustomSetupExitCode: Integer;
begin
  Result := 0;
  if not TexturePackInstalled then Result := 1;
end;
