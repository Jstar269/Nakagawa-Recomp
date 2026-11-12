; SPDX-License-Identifier: GPL-3.0-or-later
; Copyright (C) 2026 the Nakagawa Recomp authors
;
; Per-user Windows installer for Nakagawa Recomp (#2208). Built by tools/build_installer.py with the
; Inno Setup version pinned in assets/upstream/innosetup.lock.json, from the folder a release ZIP unpacks to.
; The compiler is passed: PackageDir, AppVersion, IconFile, TouchDate, TouchTime.
;
; What this installer does and does not do (the rules are pinned by tools/test_installer.py):
;   - Per user, no administrator: nothing is written under Program Files, HKLM or any all-users folder.
;   - Writes the program files, a Start-menu shortcut (and an optional desktop shortcut) with the player's
;     taskbar identity, and Inno's own per-user uninstall entry. No other registry value, no file
;     association, no PATH entry, no service, no scheduled task.
;   - Uses no network: no download, no plugin, no web request. The one DLL function it calls is kernel32's
;     GetLongPathNameW, a local file-system lookup.
;   - An upgrade in place also deletes the files the previous install's own SHA256SUMS.txt lists, the new one does
;     not, and that are unchanged since Setup installed them; nothing else in the folder is touched.
;   - The uninstaller removes what Setup installed. It never touches %LOCALAPPDATA%\Nakagawa (settings,
;     games, saves, per-title folders) unless the person ticks the box on the uninstall page; a silent
;     uninstall never deletes it.

#ifndef PackageDir
  #error PackageDir is required: the folder a release ZIP unpacks to
#endif
#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef IconFile
  #error IconFile is required
#endif
#ifndef TouchDate
  #error TouchDate is required (YYYY-MM-DD, from SOURCE_DATE_EPOCH)
#endif
#ifndef TouchTime
  #error TouchTime is required (HH:MM:SS, from SOURCE_DATE_EPOCH)
#endif

#define AppName "Nakagawa Recomp"
; The shortcut's name. Only tests override it (/DShortcutName=..., build_installer.build_installer's extra_defines), so
; a trial install on a PC that has the real shortcut never replaces or removes it. The other test-only define is
; TestProbe (the end of [Code]); a release build defines neither.
#ifndef ShortcutName
  #define ShortcutName AppName
#endif
#define AppExe "nakagawa_player.exe"
; The player's AppUserModelID (APP_USER_MODEL_ID in tools/stage_install.py, #2006).
#define AppUserModelID "Nakagawa.Recomp"

[Setup]
; Fixed for the life of the product: a changed AppId makes an upgrade a second, separate install.
AppId={{A37F65B8-FB5B-41BB-AC7B-3700FE43CB02}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Nakagawa Recomp project
AppPublisherURL=https://github.com/Jstar269/nakagawa-recomp
AppSupportURL=https://github.com/Jstar269/nakagawa-recomp/issues
AppUpdatesURL=https://github.com/Jstar269/nakagawa-recomp/releases
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
DisableDirPage=no
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile={#PackageDir}\LICENSE
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\bin\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
CloseApplications=yes
RestartApplications=no
ChangesAssociations=no
ChangesEnvironment=no
; Deterministic file times inside the installer: the release commit's time, never the clock.
TimeStampsInUTC=yes
TouchDate={#TouchDate}
TouchTime={#TouchTime}
OutputDir=.
OutputBaseFilename=nakagawa-recomp-{#AppVersion}-windows-x64-setup
SetupLogging=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion touch

[Icons]
Name: "{userprograms}\{#ShortcutName}"; Filename: "{app}\bin\{#AppExe}"; WorkingDir: "{app}\bin"; AppUserModelID: "{#AppUserModelID}"
Name: "{userdesktop}\{#ShortcutName}"; Filename: "{app}\bin\{#AppExe}"; WorkingDir: "{app}\bin"; AppUserModelID: "{#AppUserModelID}"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\{#AppExe}"; Description: "Start {#AppName}"; Flags: postinstall nowait skipifsilent runasoriginaluser

[Code]
const
  USER_DATA_FOLDER_COUNT = 5;
  { The uninstall entry Inno writes for this AppId (tools/verify_installer.py, UNINSTALL_KEY). }
  UNINSTALL_KEY = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A37F65B8-FB5B-41BB-AC7B-3700FE43CB02}_is1';
  MANIFEST_NAME = 'SHA256SUMS.txt';

var
  DeleteUserData: Boolean;
  { The previous install's own manifest, read just before the new files are copied over it. }
  PreviousPaths, PreviousDigests: TArrayOfString;
  HavePreviousManifest: Boolean;

{ The five folders of the per-user data layout: the only folders the uninstall opt-in may delete. They are the
  folders the player resolves under %LOCALAPPDATA%\Nakagawa (nk_platform_win32.c, resolve_windows_path), and
  docs/SETUP.md lists them. Anything the player keeps inside "data" (the library, saves, downloads) goes with it. }
function UserDataFolder(Index: Integer): String;
begin
  case Index of
    0: Result := 'config';
    1: Result := 'data';
    2: Result := 'cache';
    3: Result := 'logs';
    4: Result := 'saves';
  else
    Result := '';
  end;
end;

function UserDataRoot: String;
begin
  Result := ExpandConstant('{localappdata}') + '\Nakagawa';
end;

function IsReparsePoint(const Path: String): Boolean;
var
  Found: TFindRec;
begin
  { Fails closed: a path that cannot be examined counts as a link, so it is neither entered nor deleted. }
  Result := True;
  if FindFirst(Path, Found) then
  begin
    try
      Result := (Found.Attributes and FILE_ATTRIBUTE_REPARSE_POINT) <> 0;
    finally
      FindClose(Found);
    end;
  end;
end;

{ The target of a shortcut file, or '' when it cannot be read. Windows' own scripting shell reads it
  locally; nothing here touches the network. }
function ShortcutTarget(const Link: String): String;
var
  Shell, Shortcut: Variant;
begin
  Result := '';
  try
    Shell := CreateOleObject('WScript.Shell');
    Shortcut := Shell.CreateShortcut(Link);
    Result := Shortcut.TargetPath;
  except
    Result := '';
  end;
end;

{ Windows' own GetLongPathNameW (kernel32, a local file-system call; the only DLL function the script uses). }
function GetLongPathName(ShortPath: String; LongPath: String; Size: Cardinal): Cardinal;
  external 'GetLongPathNameW@kernel32.dll stdcall';

{ Path with every 8.3 short part spelt in full (a hosted runners "RUNNER~1" is "runneradmin"), or Path itself when
  it does not exist or cannot be read. }
function LongPathName(const Path: String): String;
var
  Size, Written: Cardinal;
begin
  Result := Path;
  Size := GetLongPathName(Path, '', 0);
  if Size = 0 then
    Exit;
  Result := StringOfChar(' ', Size);
  Written := GetLongPathName(Path, Result, Size);
  if (Written = 0) or (Written >= Size) then
    Result := Path
  else
    SetLength(Result, Written);
end;

{ True when A and B name the same file or folder: compared without case, as Windows does, and with the short and
  long spellings of a folder taken as one. The shell keeps a shortcut's target in long form whatever form Setup
  wrote, while the install folder keeps the form /DIR= or the wizard gave it, so a plain text compare would take this install's
  own shortcut for another copy's. }
function SamePath(const A, B: String): Boolean;
begin
  Result := CompareText(LongPathName(RemoveBackslashUnlessRoot(ExpandFileName(A))),
                        LongPathName(RemoveBackslashUnlessRoot(ExpandFileName(B)))) = 0;
end;

{ The target of an existing shortcut that points at another copy of the player, or '' when there is no
  shortcut, it is ours, or its target is gone (a stale shortcut is replaced). }
function OtherCopyShortcutTarget(const Link: String): String;
var
  Target, Ours: String;
begin
  Result := '';
  if not FileExists(Link) then
    Exit;
  Target := ShortcutTarget(Link);
  if Target = '' then
    Exit;
  Ours := ExpandConstant('{app}') + '\bin\{#AppExe}';
  if (not SamePath(Target, Ours)) and FileExists(Target) then
    Result := Target;
end;

{ Two things this installer must not do: install into a folder the developer install tool set up (it has
  its own uninstall, and two uninstallers must never share a folder), and take over the shortcut of another
  copy (tools/stage_install.py writes the same shortcut name with the same taskbar identity, so installing or
  removing either copy would replace or remove the other's shortcut). }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Target: String;
begin
  Result := '';
  if FileExists(ExpandConstant('{app}') + '\.nakagawa-install.json') then
  begin
    Result := 'This folder was set up by the developer install tool (tools/stage_install.py), which has its own uninstall. Choose another folder.';
    Exit;
  end;
  Target := OtherCopyShortcutTarget(ExpandConstant('{userprograms}\{#ShortcutName}.lnk'));
  if (Target = '') and WizardIsTaskSelected('desktopicon') then
    Target := OtherCopyShortcutTarget(ExpandConstant('{userdesktop}\{#ShortcutName}.lnk'));
  if Target <> '' then
    Result := 'A shortcut named {#ShortcutName} already starts another copy of Nakagawa (' + Target + '). Installing here would replace it, and removing either copy later would remove the shortcut of the other. Uninstall or delete that copy and its shortcut, then run Setup again.';
end;

{ Upgrading in place. Inno Setup overwrites the files the new package carries but leaves the ones an earlier release
  installed and this one dropped (a stale DLL in bin\ would be found before the system's). The previous install's
  own SHA256SUMS.txt lists the files Setup put there, so after the new files are copied Setup deletes each file that
  list names, the new SHA256SUMS.txt does not, and that is still byte-identical to what was installed. A file the
  person added or changed, bin\data, bin\portable.txt and everything outside the install folder
  (%LOCALAPPDATA%\Nakagawa included) are never touched, and nothing is deleted through a junction or link. }

function IsHexDigest(const Text: String): Boolean;
var
  Index: Integer;
begin
  Result := Length(Text) = 64;
  if Result then
    for Index := 1 to 64 do
      if Pos(Text[Index], '0123456789abcdef') = 0 then
      begin
        Result := False;
        Exit;
      end;
end;

{ One part of a manifest path, named exactly by its text. Windows drops a trailing dot or space from a part
  ("data." opens "data") and may read a part with '~' as an 8.3 short name, so such a part could name another file;
  that also covers '.' and '..'. Wildcards, stream colons, the other reserved characters, both slashes and control
  characters never occur in a package. }
function IsSafeManifestPart(const Part: String): Boolean;
var
  Index: Integer;
begin
  Result := False;
  if Part = '' then
    Exit;
  if (Part[Length(Part)] = '.') or (Part[Length(Part)] = ' ') then
    Exit;
  for Index := 1 to Length(Part) do
    if (Ord(Part[Index]) < 32) or (Ord(Part[Index]) = 127) or (Pos(Part[Index], '~*?:<>"|\/') > 0) then
      Exit;
  Result := True;
end;

{ A manifest path that stays inside the folder and names one file: relative and '/'-separated, every part safe as
  above. A package never lists Setup's own top-level files (unins*, SHA256SUMS.txt) or the person's portable data
  (bin/data, bin/portable.txt), so a list that names any of them, in any case, is not used. }
function IsSafeManifestName(const Name: String): Boolean;
var
  Lowered, Rest, Part: String;
  Split: Integer;
begin
  Result := False;
  Lowered := Lowercase(Name);
  if (Pos('unins', Lowered) = 1) or (Lowered = 'sha256sums.txt') or (Lowered = 'bin/portable.txt')
     or (Lowered = 'bin/data') or (Pos('bin/data/', Lowered) = 1) then
    Exit;
  Rest := Name;
  repeat
    Split := Pos('/', Rest);
    if Split = 0 then
    begin
      Part := Rest;
      Rest := '';
    end
    else
    begin
      Part := Copy(Rest, 1, Split - 1);
      Rest := Copy(Rest, Split + 1, Length(Rest));
    end;
    if not IsSafeManifestPart(Part) then
      Exit;
  until Split = 0;
  Result := True;
end;

{ The paths and SHA-256 values of a "<sha256>  <path>" manifest. False, with both lists empty, when the file is
  missing, is a link or cannot be read, or when any line is not of that form: then nothing is deleted. }
function ReadManifest(const FileName: String; var Paths, Digests: TArrayOfString): Boolean;
var
  Lines: TArrayOfString;
  Index, Count: Integer;
  Line, Digest, Name: String;
begin
  Result := False;
  SetArrayLength(Paths, 0);
  SetArrayLength(Digests, 0);
  if IsReparsePoint(FileName) then
    Exit;
  if not LoadStringsFromFile(FileName, Lines) then
    Exit;
  SetArrayLength(Paths, GetArrayLength(Lines));
  SetArrayLength(Digests, GetArrayLength(Lines));
  Count := 0;
  for Index := 0 to GetArrayLength(Lines) - 1 do
  begin
    Line := Lines[Index];
    if Line <> '' then
    begin
      Digest := Copy(Line, 1, 64);
      Name := Copy(Line, 67, Length(Line) - 66);
      if (Copy(Line, 65, 2) <> '  ') or (not IsHexDigest(Digest)) or (not IsSafeManifestName(Name)) then
      begin
        SetArrayLength(Paths, 0);
        SetArrayLength(Digests, 0);
        Exit;
      end;
      Paths[Count] := Name;
      Digests[Count] := Digest;
      Count := Count + 1;
    end;
  end;
  SetArrayLength(Paths, Count);
  SetArrayLength(Digests, Count);
  Result := Count > 0;
end;

{ True when Dir, or any folder or file from Dir down to Dir\Relative, is a junction or symbolic link, or cannot be
  examined. }
function LinkOnPath(const Dir, Relative: String): Boolean;
var
  Current, Rest: String;
  Split: Integer;
begin
  Result := IsReparsePoint(Dir);
  Current := Dir;
  Rest := Relative;
  while (not Result) and (Rest <> '') do
  begin
    Split := Pos('\', Rest);
    if Split = 0 then
    begin
      Current := Current + '\' + Rest;
      Rest := '';
    end
    else
    begin
      Current := Current + '\' + Copy(Rest, 1, Split - 1);
      Rest := Copy(Rest, Split + 1, Length(Rest));
    end;
    Result := IsReparsePoint(Current);
  end;
end;

function FileHasDigest(const Path, Digest: String): Boolean;
begin
  try
    Result := Lowercase(GetSHA256OfFile(Path)) = Digest;
  except
    Result := False;
  end;
end;

{ Deletes the files OldPaths lists and Dir's own (new) SHA256SUMS.txt does not, each only when it is a plain file
  still equal to its OldDigests value and no link lies on its path; then removes the folders that are left empty,
  never Dir itself. Names compare without case, as Windows does. }
procedure RemoveDroppedFiles(const Dir: String; const OldPaths, OldDigests: TArrayOfString);
var
  NewPaths, NewDigests: TArrayOfString;
  Carried: TStringList;
  Index, Found: Integer;
  Relative, Path, Folder: String;
begin
  if not ReadManifest(Dir + '\' + MANIFEST_NAME, NewPaths, NewDigests) then
    Exit;
  Carried := TStringList.Create;
  try
    for Index := 0 to GetArrayLength(NewPaths) - 1 do
      Carried.Add(Lowercase(NewPaths[Index]));
    Carried.Sorted := True;
    for Index := 0 to GetArrayLength(OldPaths) - 1 do
    begin
      if not Carried.Find(Lowercase(OldPaths[Index]), Found) then
      begin
        Relative := OldPaths[Index];
        StringChangeEx(Relative, '/', '\', True);
        Path := Dir + '\' + Relative;
        if FileExists(Path) and (not LinkOnPath(Dir, Relative)) then
          if FileHasDigest(Path, OldDigests[Index]) then
            if DeleteFile(Path) then
            begin
              Folder := ExtractFileDir(Path);
              while (Length(Folder) > Length(Dir)) and RemoveDir(Folder) do
                Folder := ExtractFileDir(Folder);
            end;
      end;
    end;
  finally
    Carried.Free;
  end;
end;

{ True only when the uninstall entry of this AppId says Setup installed into Dir and Setup's uninstall log is there:
  then Dir's SHA256SUMS.txt lists files Setup itself installed, not an unpacked ZIP or someone else's files. }
function IsSetupInstallIn(const Dir: String): Boolean;
var
  Location: String;
begin
  Result := False;
  if not FileExists(Dir + '\unins000.dat') then
    Exit;
  if RegQueryStringValue(HKCU, UNINSTALL_KEY, 'InstallLocation', Location) then
    Result := SamePath(Location, Dir);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    HavePreviousManifest := False;
    if IsSetupInstallIn(ExpandConstant('{app}')) then
      HavePreviousManifest := ReadManifest(ExpandConstant('{app}') + '\' + MANIFEST_NAME, PreviousPaths, PreviousDigests);
  end
  else if (CurStep = ssPostInstall) and HavePreviousManifest then
    RemoveDroppedFiles(ExpandConstant('{app}'), PreviousPaths, PreviousDigests);
end;

{ Deletes Dir and what is in it without ever following a junction or symbolic link: a link inside the tree is
  removed as a link (its target is left alone), and a folder is entered only when it is a real folder. }
function DeleteTreeNoLinks(const Dir: String; Depth: Integer): Boolean;
var
  Found: TFindRec;
  Path: String;
  IsDir, IsLink: Boolean;
begin
  Result := True;
  if Depth > 64 then
  begin
    Result := False;
    Exit;
  end;
  if FindFirst(Dir + '\*', Found) then
  begin
    try
      repeat
        if (Found.Name <> '.') and (Found.Name <> '..') then
        begin
          Path := Dir + '\' + Found.Name;
          IsDir := (Found.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0;
          IsLink := (Found.Attributes and FILE_ATTRIBUTE_REPARSE_POINT) <> 0;
          if IsLink then
          begin
            if IsDir then
            begin
              if not RemoveDir(Path) then
                Result := False;
            end
            else if not DeleteFile(Path) then
              Result := False;
          end
          else if IsDir then
          begin
            if not DeleteTreeNoLinks(Path, Depth + 1) then
              Result := False;
          end
          else if not DeleteFile(Path) then
          begin
            { A read-only file: DelTree on a single file clears the attribute first. }
            if not DelTree(Path, False, True, False) then
              Result := False;
          end;
        end;
      until not FindNext(Found);
    finally
      FindClose(Found);
    end;
  end;
  if not RemoveDir(Dir) then
    Result := False;
end;

{ Deletes exactly the five data folders under %LOCALAPPDATA%\Nakagawa, and that folder if it is then empty.
  Nothing is deleted through a junction or symbolic link, at the top or anywhere inside. }
procedure RemoveUserDataIn(const Root: String);
var
  Index: Integer;
  Folder: String;
begin
  if (not DirExists(Root)) or IsReparsePoint(Root) then
    Exit;
  for Index := 0 to USER_DATA_FOLDER_COUNT - 1 do
  begin
    Folder := Root + '\' + UserDataFolder(Index);
    if DirExists(Folder) and (not IsReparsePoint(Folder)) then
      DeleteTreeNoLinks(Folder, 0);
  end;
  RemoveDir(Root);
end;

procedure RemoveUserData;
begin
  RemoveUserDataIn(UserDataRoot);
end;

{ True while the player is running from this install. Only the player's own program file is probed: a running
  program's file cannot be opened for writing. Inno's uninstaller does not close programs the way Setup does, so it would remove the
  shortcut and the uninstall entry and leave the locked files behind. }
function PlayerIsRunning: Boolean;
var
  Exe: String;
begin
  Exe := ExpandConstant('{app}') + '\bin\{#AppExe}';
  Result := FileExists(Exe) and (not SaveStringToFile(Exe, '', True));
end;

{ The uninstall page: one box, unchecked. A silent uninstall skips it and keeps the data. }
function InitializeUninstall(): Boolean;
var
  Form: TSetupForm;
  Heading, Detail: TNewStaticText;
  Box: TNewCheckBox;
  Ok, Cancel: TNewButton;
begin
  Result := True;
  DeleteUserData := False;
  { Nothing is changed while the player runs: a silent uninstall stops, an interactive one asks to close it. }
  while PlayerIsRunning do
  begin
    if UninstallSilent then
    begin
      Result := False;
      Exit;
    end;
    if MsgBox('Nakagawa Recomp is running. Close it, then choose Retry.', mbError, MB_RETRYCANCEL) <> IDRETRY then
    begin
      Result := False;
      Exit;
    end;
  end;
  if UninstallSilent then
    Exit;
  Form := CreateCustomForm(ScaleX(440), ScaleY(210), False, True);
  try
    Form.Caption := 'Uninstall Nakagawa Recomp';
    Heading := TNewStaticText.Create(Form);
    Heading.Parent := Form;
    Heading.Left := ScaleX(16);
    Heading.Top := ScaleY(16);
    Heading.Width := Form.ClientWidth - ScaleX(32);
    Heading.AutoSize := False;
    Heading.WordWrap := True;
    Heading.Height := ScaleY(48);
    Heading.Caption := 'This removes the program. Your imported games, saves and settings stay in ' + UserDataRoot + ' unless you tick the box below.';
    Box := TNewCheckBox.Create(Form);
    Box.Parent := Form;
    Box.Left := ScaleX(16);
    Box.Top := ScaleY(72);
    Box.Width := Form.ClientWidth - ScaleX(32);
    Box.Height := ScaleY(20);
    Box.Caption := 'Also delete my games, saves and settings';
    Box.Checked := False;
    Detail := TNewStaticText.Create(Form);
    Detail.Parent := Form;
    Detail.Left := ScaleX(36);
    Detail.Top := ScaleY(98);
    Detail.Width := Form.ClientWidth - ScaleX(52);
    Detail.AutoSize := False;
    Detail.WordWrap := True;
    Detail.Height := ScaleY(48);
    Detail.Caption := 'Deletes the config, data, cache, logs and saves folders. Saves cannot be recovered. Your disc images elsewhere are never touched.';
    Ok := TNewButton.Create(Form);
    Ok.Parent := Form;
    Ok.Caption := 'Uninstall';
    Ok.ModalResult := mrOk;
    Ok.Left := Form.ClientWidth - ScaleX(184);
    Ok.Top := Form.ClientHeight - ScaleY(40);
    Ok.Width := ScaleX(80);
    Ok.Height := ScaleY(25);
    Ok.Default := True;
    Cancel := TNewButton.Create(Form);
    Cancel.Parent := Form;
    Cancel.Caption := 'Cancel';
    Cancel.ModalResult := mrCancel;
    Cancel.Left := Form.ClientWidth - ScaleX(96);
    Cancel.Top := Form.ClientHeight - ScaleY(40);
    Cancel.Width := ScaleX(80);
    Cancel.Height := ScaleY(25);
    Cancel.Cancel := True;
    Form.ActiveControl := Ok;
    Result := Form.ShowModal = mrOk;
    if Result and Box.Checked then
    begin
      { Saves are not recoverable: ask once more, with "No" the default. }
      if MsgBox('Delete your games, saves and settings in ' + UserDataRoot + '? This cannot be undone.',
                mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DeleteUserData := True;
    end;
  finally
    Form.Free;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if (CurUninstallStep = usPostUninstall) and DeleteUserData and (not UninstallSilent) then
    RemoveUserData;
end;

#ifdef TestProbe
{ Tests only (tools/test_installer.py, RealCompilerTests); a release build never defines TestProbe. /PROBE=userdata
  runs RemoveUserDataIn on /ROOT=; /PROBE=upgrade runs RemoveDroppedFiles on /ROOT= with /OLDSUMS= as the previous
  install's manifest; /PROBE=samepath writes same or different to /OUT= for SamePath(/A=, /B=). It then writes the
  probe's name to /DONE= and exits before the wizard, so nothing is installed, and nothing at all happens without a
  known /PROBE=. }
function InitializeSetup(): Boolean;
var
  Probe: String;
  Paths, Digests: TArrayOfString;
begin
  Result := False;
  Probe := ExpandConstant('{param:PROBE|}');
  if Probe = 'userdata' then
    RemoveUserDataIn(ExpandConstant('{param:ROOT|}'))
  else if Probe = 'upgrade' then
  begin
    if ReadManifest(ExpandConstant('{param:OLDSUMS|}'), Paths, Digests) then
      RemoveDroppedFiles(ExpandConstant('{param:ROOT|}'), Paths, Digests);
  end
  else if Probe = 'samepath' then
  begin
    if SamePath(ExpandConstant('{param:A|}'), ExpandConstant('{param:B|}')) then
      SaveStringToFile(ExpandConstant('{param:OUT|}'), 'same', False)
    else
      SaveStringToFile(ExpandConstant('{param:OUT|}'), 'different', False);
  end
  else
    Exit;
  SaveStringToFile(ExpandConstant('{param:DONE|}'), Probe, False);
end;
#endif
