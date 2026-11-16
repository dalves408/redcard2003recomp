{ Hi-res texture pack: download (or use texture.bin beside Setup), verify, extract.
  Adapted from the NHL Hitz Pro installer for RedCard's single texture.bin.
  Needs the pinned Texture* defines from texture_payload.iss and a [Tasks] entry "hires".
  redcard_texture_download.exe does the transfer: resumable (partial downloads survive a
  closed Setup), and it reports a file only once its size and SHA-256 match. }
var
  TextureProgress: TOutputProgressWizardPage;
  TextureTransferActive, TextureTransferCancelled, TextureExtractActive: Boolean;
  TexturePackInstalled, TexturePackAlreadyCurrent: Boolean;
  TextureUsedCache, TextureCacheChoiceAsked, TextureDeleteCacheOnExit: Boolean;
  TextureLastUiTick: LongWord;
  TexturePayloadPath, TextureCacheDir, TextureCancelFile, TextureError: String;
  TextureRatePhase: String;
  TextureRateTick: LongWord;
  TextureRateBytes, TextureRatePerSecond: Int64;
  TextureRateAvailable: Boolean;

function TextureSetupProcessId: LongWord;
  external 'GetCurrentProcessId@kernel32.dll stdcall';
function TextureTick: LongWord;
  external 'GetTickCount@kernel32.dll stdcall';

function TextureForceRepair: Boolean;
begin
  Result := ExpandConstant('{param:REPAIRTEXTURES|0}') = '1';
end;

{ The pack in Dir\texture_pack is this version and complete: manifest.txt has the pinned
  hash and every file it lists exists with its listed size. }
function TextureInstalledPackCurrent(Dir: String): Boolean;
var
  Manifest, Rest, Relative: String;
  Lines: TArrayOfString;
  I, Space: Integer;
  ExpectedSize, ActualSize: Int64;
begin
  Result := False;
  if Dir = '' then Exit;
  Dir := AddBackslash(Dir) + 'texture_pack\';
  Manifest := Dir + 'manifest.txt';
  if not FileExists(Manifest) then Exit;
  try
    if CompareText(GetSHA256OfFile(Manifest), '{#TextureManifestSHA256}') <> 0 then Exit;
    if not LoadStringsFromFile(Manifest, Lines) then Exit;
    if GetArrayLength(Lines) <> {#TextureFileCount} then Exit;
    for I := 0 to GetArrayLength(Lines) - 1 do begin
      Rest := Lines[I];
      Space := Pos(' ', Rest);
      if Space <> 65 then Exit;
      Delete(Rest, 1, Space);
      Space := Pos(' ', Rest);
      if Space <= 1 then Exit;
      ExpectedSize := StrToInt64Def(Copy(Rest, 1, Space - 1), -1);
      Relative := Copy(Rest, Space + 1, Length(Rest));
      StringChangeEx(Relative, '/', '\', True);
      if (Relative = '') or (ExpectedSize < 0) then Exit;
      if not FileSize64(Dir + Relative, ActualSize) then Exit;
      if ActualSize <> ExpectedSize then Exit;
    end;
    Result := True;
  except
    Result := False;
  end;
end;

{ The game also loads %LOCALAPPDATA%\RedCard2003\texture_pack (before the one in its folder). }
function TextureAnyPackCurrent(AppDir: String): Boolean;
begin
  Result := TextureInstalledPackCurrent(AppDir) or
    TextureInstalledPackCurrent(ExpandConstant('{localappdata}\RedCard2003'));
end;

function TexturePackNeedsInstall: Boolean;
begin
  Result := TextureForceRepair or not TextureAnyPackCurrent(WizardDirValue);
  if not Result then Log('The current texture pack is installed; the texture option is hidden.');
end;

function WantTexturePack: Boolean;
begin
#ifdef TextureAlways
  Result := True;      { the standalone add-on installs nothing else }
#else
  Result := WizardIsTaskSelected('hires') or
    (CompareText(ExpandConstant('{param:HIRES|no}'), 'yes') = 0);
#endif
end;

function TextureLocalPart: String;
begin
  Result := AddBackslash(ExpandConstant('{src}')) + 'texture.bin';
end;

procedure TextureCancelButtonClick(var Cancel, Confirm: Boolean);
begin
  if TextureTransferActive then begin
    TextureTransferCancelled := True;
    SaveStringToFile(TextureCancelFile, 'cancel', False);
    if TextureExtractActive then
      TextureProgress.SetText('Stopping extraction...', 'The verified download is kept for next time.')
    else
      TextureProgress.SetText('Stopping download...', 'Saved progress will be resumed next time.');
    Cancel := False;
    Confirm := False;
  end;
end;

function TextureNetworkPhase(Phase: String): Boolean;
begin
  Result := (Phase = 'DOWNLOADING') or (Phase = 'RESUMING') or (Phase = 'RESTARTING');
end;

function TextureFormatBytes(Bytes: Int64): String;
begin
  if Bytes >= 1000000000 then Result := Format('%.2f GB', [Bytes / 1000000000.0])
  else if Bytes >= 1000000 then Result := Format('%.1f MB', [Bytes / 1000000.0])
  else if Bytes >= 1000 then Result := Format('%.1f KB', [Bytes / 1000.0])
  else Result := IntToStr(Bytes) + ' B';
end;

procedure TextureUpdateDownloadRate(Phase: String; Done: Int64; Tick: LongWord);
begin
  if not TextureNetworkPhase(Phase) or (Phase <> TextureRatePhase) or
     (Tick < TextureRateTick) or (Done < TextureRateBytes) then begin
    TextureRatePhase := Phase;
    TextureRateTick := Tick;
    TextureRateBytes := Done;
    TextureRatePerSecond := 0;
    TextureRateAvailable := False;
  end else if Tick - TextureRateTick >= 1000 then begin
    TextureRatePerSecond := (Done - TextureRateBytes) * 1000 div (Tick - TextureRateTick);
    TextureRateAvailable := True;
    TextureRateTick := Tick;
    TextureRateBytes := Done;
  end;
end;

function TextureProgressDetails(Phase: String; Done, Total: Int64): String;
var Percent: Extended; Speed: String;
begin
  if Done < 0 then Done := 0;
  if Done > Total then Done := Total;
  Percent := 0;
  if Total > 0 then Percent := Done * 100.0 / Total;
  if TextureNetworkPhase(Phase) then begin
    if TextureRateAvailable then Speed := TextureFormatBytes(TextureRatePerSecond) + '/s'
    else Speed := 'Measuring speed...';
    Result := Format('%.1f%% - %.1f / %.1f MB - %s', [Percent, Done / 1000000.0, Total / 1000000.0, Speed]);
  end else if Phase = 'VERIFYING' then
    Result := Format('%s - %.1f%% verified', [TextureFormatBytes(Total), Percent])
  else Result := 'Size: ' + TextureFormatBytes(Total);
end;

procedure TextureDownloadLog(const S: String; const Error, FirstLine: Boolean);
var
  Rest, Phase, LabelText: String;
  K: Integer;
  Done, Total: Int64;
begin
  if Error then begin TextureError := S; Log(S); Exit; end;
  if Copy(S, 1, 9) = 'PROGRESS ' then begin
    Rest := Copy(S, 10, Length(S));
    K := Pos(' ', Rest);
    Done := StrToInt64Def(Copy(Rest, 1, K - 1), 0);
    Delete(Rest, 1, K);
    K := Pos(' ', Rest);
    Total := StrToInt64Def(Copy(Rest, 1, K - 1), 0);
    Phase := Copy(Rest, K + 1, Length(Rest));
    if TextureTransferCancelled then LabelText := 'Stopping download...'
    else if Phase = 'WAITING' then LabelText := 'Waiting for another texture download to finish...'
    else if Phase = 'RESUMING' then LabelText := 'Resuming the texture pack download...'
    else if Phase = 'VERIFYING' then LabelText := 'Checking the texture pack...'
    else if Phase = 'RESTARTING' then LabelText := 'The server cannot resume this copy. Restarting the download...'
    else if Phase = 'CHECKING' then LabelText := 'Looking for a texture pack beside Setup...'
    else LabelText := 'Downloading the texture pack...';
    TextureUpdateDownloadRate(Phase, Done, TextureTick);
    TextureProgress.SetText(LabelText, TextureProgressDetails(Phase, Done, Total));
    TextureProgress.SetProgress(Done div 1024, Total div 1024);
  end else begin
    Log(S);
    if S = 'READY local' then begin
      TexturePayloadPath := TextureLocalPart;
      TextureUsedCache := False;
    end else if S = 'READY cache' then begin
      TexturePayloadPath := TextureCacheDir + '\texture.bin';
      TextureUsedCache := True;
    end
    else if Copy(S, 1, 6) = 'ERROR ' then TextureError := Copy(S, 7, Length(S));
  end;
end;

{ Called from PrepareToInstall: download and verify before any file is installed, so a
  stopped or failed download leaves Setup on its Ready page with Retry. '' = ready. }
function PrepareTexturePayload(OtherRequiredBytes: Int64): String;
var
  Params, LocalParam: String;
  Code: Integer;
  Started, RetryWithoutLocal: Boolean;
  FreeBytes, TotalBytes: Int64;
begin
  Result := '';
  if not WantTexturePack then Exit;
  TexturePackAlreadyCurrent := not TextureForceRepair and TextureAnyPackCurrent(WizardDirValue);
  TexturePackInstalled := TexturePackAlreadyCurrent;
  if TexturePackAlreadyCurrent then begin
    Log('The current texture pack is already installed; nothing to download.');
    Exit;
  end;
  if not GetSpaceOnDisk64(AddBackslash(ExtractFileDrive(WizardDirValue)), FreeBytes, TotalBytes) then begin
    Result := 'Could not check the free space on the game''s drive.';
    Exit;
  end;
  if FreeBytes < ({#TextureExtractedBytes} + OtherRequiredBytes) then begin
    Result := 'There is not enough free space for the hi-res textures (' +
      TextureFormatBytes({#TextureExtractedBytes}) + ') and the game files. Free some space, or untick the texture pack.';
    Exit;
  end;
  MapArchiveExtensions('.bin', '.7z');
  if not FileExists(ExpandConstant('{tmp}\redcard_texture_download.exe')) then
    ExtractTemporaryFile('redcard_texture_download.exe');
  TextureCacheDir := ExpandConstant('{#TextureCacheRoot}') + '\{#TextureSHA256}';
  TextureCancelFile := ExpandConstant('{tmp}\texture-download.cancel');
  if TextureProgress = nil then
    TextureProgress := CreateOutputProgressPage('Hi-res texture pack', 'Getting the 4x texture pack ({#TextureVersion}).');
  if FileExists(TextureLocalPart) then
    TextureProgress.SetText('Checking the texture pack beside Setup...', '')
  else if FileExists(TextureCacheDir + '\texture.bin.part') then
    TextureProgress.SetText('Resuming the texture pack download...', 'Saved progress will be used.')
  else
    TextureProgress.SetText('The texture pack will be downloaded (' + TextureFormatBytes({#TextureBytes}) + ').',
      'You can stop and resume it later.');
  TextureProgress.SetProgress(0, 0);
  TextureProgress.Show;
  TextureTransferActive := True;
  TextureTransferCancelled := False;
  WizardForm.CancelButton.Enabled := True;
  WizardForm.CancelButton.Visible := True;
  LocalParam := ' --local ' + AddQuotes(TextureLocalPart);
  try
    repeat
      DeleteFile(TextureCancelFile);
      TexturePayloadPath := '';
      TextureUsedCache := False;
      TextureError := '';
      TextureRatePhase := '';
      TextureRateAvailable := False;
      Params := '--url ' + AddQuotes('{#TextureURL}') +
                ' --cache ' + AddQuotes(TextureCacheDir) +
                ' --sha256 {#TextureSHA256} --bytes {#TextureBytes}' +
                ' --cancel ' + AddQuotes(TextureCancelFile) +
                ' --parent ' + IntToStr(TextureSetupProcessId) + LocalParam;
      Code := 99;
      Started := ExecAndLogOutput(ExpandConstant('{tmp}\redcard_texture_download.exe'), Params, '',
                                 SW_HIDE, ewWaitUntilTerminated, Code, @TextureDownloadLog);
      RetryWithoutLocal := False;
      if Started and (Code = 5) and not TextureTransferCancelled then begin
        if WizardSilent then RetryWithoutLocal := True
        else RetryWithoutLocal := MsgBox('The texture.bin beside Setup is not this texture pack.' + #13#10#13#10 +
          'Download the correct copy instead? Your file will be left as it is.', mbConfirmation, MB_YESNO) = IDYES;
        if RetryWithoutLocal then LocalParam := '';
      end;
    until not RetryWithoutLocal;
    if TextureTransferCancelled or (Code = 2) then
      Result := 'The texture download was stopped. Its progress is saved: click Back and Install to resume, ' +
        'untick the texture pack to install without it, or Cancel to close Setup.'
    else if not Started or (Code <> 0) or (TexturePayloadPath = '') then begin
      if TextureError = '' then TextureError := 'The texture pack could not be downloaded.';
      Result := TextureError + #13#10#13#10 + 'Click Back and Install to try again (a partial download is resumed), ' +
        'or untick the texture pack to install without it.';
    end;
  finally
    TextureTransferActive := False;
    TextureProgress.Hide;
  end;
end;

procedure TextureFinishCacheChoice;
begin
  { Only after a successful install, and only for Setup's own download cache. }
  if not TexturePackInstalled or TexturePackAlreadyCurrent or TextureCacheChoiceAsked or
     not TextureUsedCache then Exit;
  TextureCacheChoiceAsked := True;
  if ExpandConstant('{param:KEEPTEXTUREBIN|}') = '0' then TextureDeleteCacheOnExit := True
  else if (ExpandConstant('{param:KEEPTEXTUREBIN|}') = '1') or WizardSilent then TextureDeleteCacheOnExit := False
  else
    TextureDeleteCacheOnExit := MsgBox(
      'Keep the downloaded texture archive (' + TextureFormatBytes({#TextureBytes}) + ') for repairs or reinstalls?' + #13#10#13#10 +
      'Yes keeps it. No deletes it when Setup closes; the textures stay installed.',
      mbConfirmation, MB_YESNO) = IDNO;
end;

procedure TextureCleanupOnExit;
begin
  if TextureDeleteCacheOnExit and TexturePackInstalled and TextureUsedCache and FileExists(TexturePayloadPath) then
    if not DeleteFile(TexturePayloadPath) then Log('The cached texture archive may still be in use.');
end;

function TextureExtractionProgress(const ArchiveName, FileName: String; const Progress, ProgressMax: Int64): Boolean;
var Tick: LongWord;
begin
  Tick := TextureTick;
  if (Tick - TextureLastUiTick >= 100) or (Progress = ProgressMax) then begin
    TextureLastUiTick := Tick;
    TextureProgress.SetText('Installing the hi-res textures...', ExtractFileName(FileName));
    TextureProgress.SetProgress(Progress div 1024, ProgressMax div 1024);
  end;
  Result := not TextureTransferCancelled;
end;

const
  TexturePackKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{{#PackGuid}}_is1';

{ Programs and Features shows the pack and its version. The game's own uninstaller
  removes texture_pack ([UninstallDelete]); this entry is removed with it. }
procedure RegisterTexturePackEntry(AppDir: String);
var SizeKB: Int64;
begin
  AppDir := RemoveBackslashUnlessRoot(AppDir);
  if AppDir = '' then Exit;
  if not RegWriteStringValue(HKCU, TexturePackKey, 'DisplayName', 'RedCard 2003 Hi-Res Textures') then begin
    Log('Could not register the hi-res texture pack in Programs and Features.');
    Exit;
  end;
  RegWriteStringValue(HKCU, TexturePackKey, 'DisplayVersion', '{#TextureVersion}');
  RegWriteStringValue(HKCU, TexturePackKey, 'Publisher', 'dalves408/redcard2003recomp');
  RegWriteStringValue(HKCU, TexturePackKey, 'URLInfoAbout', 'https://github.com/dalves408/redcard2003recomp');
  RegWriteStringValue(HKCU, TexturePackKey, 'InstallLocation', AddBackslash(AppDir) + 'texture_pack\');
  RegWriteStringValue(HKCU, TexturePackKey, 'Inno Setup: App Path', AppDir);
  RegWriteStringValue(HKCU, TexturePackKey, 'DisplayIcon', AddBackslash(AppDir) + 'redcard_2003.ico');
  RegWriteDWordValue(HKCU, TexturePackKey, 'SystemComponent', 1);
  SizeKB := {#TextureExtractedBytes} div 1024;
  RegWriteDWordValue(HKCU, TexturePackKey, 'EstimatedSize', SizeKB);
end;

{ ssPostInstall: extract into a staging folder, check it against the manifest, then swap it
  in for any older texture_pack (kept until the new one is in place). }
procedure InstallTexturePack;
var
  AppDir, PackDir, Stage, Backup: String;
  OldMoved: Boolean;
begin
  if not WantTexturePack or TexturePackAlreadyCurrent then Exit;
  if TexturePayloadPath = '' then RaiseException('The texture archive was not prepared.');
  AppDir := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  PackDir := AddBackslash(AppDir) + 'texture_pack';
  Stage := AddBackslash(AppDir) + '.texture-stage-' + IntToStr(TextureSetupProcessId);
  Backup := PackDir + '.previous-' + IntToStr(TextureSetupProcessId);
  if DirExists(Stage) then DelTree(Stage, True, True, True);
  if not CreateDir(Stage) then RaiseException('Cannot create the texture staging folder.');
  OldMoved := False;
  TextureTransferCancelled := False;
  TextureExtractActive := True;
  TextureTransferActive := True;
  TextureLastUiTick := 0;
  TextureProgress.SetText('Installing the hi-res textures...', '');
  TextureProgress.SetProgress(0, 0);
  TextureProgress.Show;
  WizardForm.CancelButton.Enabled := True;
  WizardForm.CancelButton.Visible := True;
  try
    try
      ExtractArchive(TexturePayloadPath, Stage, '', True, @TextureExtractionProgress);
      if TextureTransferCancelled or not TextureInstalledPackCurrent(Stage) then
        RaiseException('The extracted texture pack is incomplete.');
      if DirExists(PackDir) then begin
        if not RenameFile(PackDir, Backup) then RaiseException('Cannot move the old texture folder.');
        OldMoved := True;
      end;
      if not RenameFile(Stage + '\texture_pack', PackDir) then begin
        if OldMoved and not RenameFile(Backup, PackDir) then
          RaiseException('Cannot restore the previous texture folder. It is saved at ' + Backup);
        RaiseException('Cannot move the texture pack into the game folder.');
      end;
      TexturePackInstalled := True;
      if OldMoved and not DelTree(Backup, True, True, True) then Log('Old texture folder remains at ' + Backup);
      RegisterTexturePackEntry(AppDir);
      Log('Hi-res texture pack {#TextureVersion} installed.');
    except
      if TextureTransferCancelled then
        SuppressibleMsgBox('Installing the hi-res textures was stopped. The game is installed without them; ' +
          'the downloaded archive is kept, so running Setup again installs them without downloading.', mbInformation, MB_OK, IDOK)
      else
        SuppressibleMsgBox('The hi-res textures could not be installed: ' + GetExceptionMessage + #13#10#13#10 +
          'The game is installed without them. Run Setup again to retry.', mbError, MB_OK, IDOK);
    end;
  finally
    DelTree(Stage, True, True, True);
    TextureTransferActive := False;
    TextureExtractActive := False;
    TextureProgress.Hide;
  end;
end;
