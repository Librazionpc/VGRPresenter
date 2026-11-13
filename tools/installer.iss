; ============================================================================
; VGRPresenter — Windows installer (Inno Setup 6).
;
; Compiled by tools/make_installer.sh, which passes the version:
;   ISCC.exe /DAPP_VERSION=0.0.1 tools\installer.iss
;
; Design decisions (the whole point: a user downloads ONE file and is running
; in a minute — no tools, no PATH, no Qt, nothing to configure):
;
;   * Everything the app needs is INSIDE the installer: the exe plus every
;     Qt DLL/plugin and the MinGW runtime, exactly as tools/make_portable.sh
;     deployed them into dist/VGRPresenter/.
;   * PER-USER install (PrivilegesRequired=lowest): installs to
;     %LocalAppData%\Programs\VGRPresenter with NO admin prompt, and
;     uninstalls cleanly from "Apps & features".
;   * Start-menu shortcut + optional desktop icon.
;   * .vgr file association (double-click a show file opens the app).
;   * SEED DATA: files the operator placed in installer/seed-data/roaming/
;     and /local/ are copied into the app's real data folders — but ONLY
;     files that don't exist yet (onlyifdoesntexist), so an existing user's
;     data is never overwritten and uninstall never deletes user data.
;     Both halves are REFRESHED BY THE BUILD (tools/make_installer.sh): the
;     design catalogs are generated from the engine (bps_cli --dump-catalogs)
;     and the configuration is snapshotted from the operator's live app.
;   * UPDATING WITHOUT RECONFIGURING: an upgrade keeps settings.json and
;     kernel.json. New defaults reach an install through the SHIPPED BASELINE
;     ({app}\assets\settings.shipped.json, replaced on every install), which the
;     app adopts only for settings that install has no value of its own for —
;     every existing choice wins. Only DERIVED state is cleared (see
;     [InstallDelete]): the app's payload directories, logs, and the thumbnail/
;     scene cache. Nothing the operator configured is deleted to make an update
;     take effect.
;   * User data lives OUTSIDE the install dir (settings + libraries in
;     %AppData%\VGR and %LocalAppData%\bps), so uninstall/reinstall/update
;     keeps everything.
; ============================================================================

#ifndef APP_VERSION
  #define APP_VERSION "0.0.0"
#endif

[Setup]
AppId={{C7B91E42-5D3A-4E9F-8A21-63F0B4D9E101}
AppName=VGRPresenter
AppVersion={#APP_VERSION}
AppVerName=VGRPresenter {#APP_VERSION}
AppPublisher=VGR
DefaultDirName={localappdata}\Programs\VGRPresenter
DefaultGroupName=VGRPresenter
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ChangesAssociations=yes
CloseApplications=yes
RestartApplications=no
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=VGRPresenter
UninstallDisplayIcon={app}\appVGRPresenterUI.exe
SetupIconFile=..\assets\app-icon.ico
OutputDir=..\dist
OutputBaseFilename=VGRPresenter-Setup-{#APP_VERSION}
VersionInfoVersion={#APP_VERSION}
VersionInfoProductName=VGRPresenter
VersionInfoDescription=VGRPresenter Setup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
; CLEAN REINSTALL (off by default): an old settings / kernel / library / cache folder left by an earlier
; build can make an installed app behave differently from a fresh one, in ways nobody can explain
; (a control that works on one laptop and does nothing on another). Ticking this ERASES everything the
; app ever stored on this computer — the old app files, settings, libraries, the media index, every
; cache including Qt's own — and then installs from scratch, so the app starts from the shipped
; baseline plus the seed data below: exactly what a brand-new machine gets. Recordings are NEVER
; touched. Silent installs: /TASKS="freshstart", or simply pass /FRESH.
Name: "freshstart"; Description: "Clean reinstall: erase ALL old VGRPresenter app files, settings, libraries and caches, then install from scratch"; GroupDescription: "If the app misbehaves after an update:"; Flags: unchecked

; ---- A CLEAN UPDATE: payload + derived state only -----------------------------
; Each build deploys a COMPLETE dist (tools/make_portable.sh removes the old
; one first), so anything listed here is about to be re-extracted. Deleting it
; first guarantees a file this build no longer ships — an older Qt plugin, a
; stale image codec, an engine log from a previous version — cannot linger and
; make a fixed build look broken. Removing the whole dir is what a "clear the
; app folder" would do; this is the same thing spelled out, so the uninstaller
; and the user's data (which never lives here) are untouched.
[InstallDelete]
Type: files;          Name: "{app}\*.dll"
Type: filesandordirs; Name: "{app}\qml"
Type: filesandordirs; Name: "{app}\qmltooling"
Type: filesandordirs; Name: "{app}\platforms"
Type: filesandordirs; Name: "{app}\imageformats"
Type: filesandordirs; Name: "{app}\styles"
Type: filesandordirs; Name: "{app}\multimedia"
Type: filesandordirs; Name: "{app}\networkinformation"
Type: filesandordirs; Name: "{app}\tls"
Type: filesandordirs; Name: "{app}\generic"
Type: filesandordirs; Name: "{app}\iconengines"
Type: filesandordirs; Name: "{app}\recordings"
Type: filesandordirs; Name: "{app}\assets"
Type: filesandordirs; Name: "{app}\tools"
Type: filesandordirs; Name: "{app}\logs"
Type: filesandordirs; Name: "{app}\enginedata"
Type: filesandordirs; Name: "{app}\crashes"
; Derived caches in the user-data folder: thumbnails and the scene cache are
; rebuilt from the media on demand. They are NEVER configuration — settings.json,
; kernel.json, the libraries and the media index are all left exactly as they are.
Type: filesandordirs; Name: "{localappdata}\bps\cache"

[Files]
; The whole deployed app (exe + Qt + MinGW runtime + plugins). Transient state
; the smoke test may have written into dist/ is excluded, and the build script
; also cleans it before compiling.
Source: "..\dist\VGRPresenter\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion; Excludes: "logs\*,enginedata\*,crashes\*"

; ---- Seed data (REFRESHED BY tools/make_installer.sh on every build) ---------
; The catalogs are generated from the engine, and settings/kernel are snapshotted
; from the operator's machine (see installer/README.md). ONLY copies what the user
; does not already have; never removed on uninstall. The settings BASELINE is not
; here: it ships inside the app payload as {app}\assets\settings.shipped.json, so
; every install gets the new one and merges it under the user's own choices.
Source: "..\installer\seed-data\roaming\*"; DestDir: "{userappdata}\VGR\VGRPresenter"; Flags: recursesubdirs createallsubdirs skipifsourcedoesntexist onlyifdoesntexist uninsneveruninstall ignoreversion
Source: "..\installer\seed-data\local\*"; DestDir: "{localappdata}\bps"; Flags: recursesubdirs createallsubdirs skipifsourcedoesntexist onlyifdoesntexist uninsneveruninstall ignoreversion
; The Shows library (song/sermon categories) — its own seed path: ShowService's
; default library lives in Documents\VGR Presenter\Shows, not roaming or local.
; Bundled by tools/make_installer.sh from whichever machine builds the release.
Source: "..\installer\seed-data\shows\*"; DestDir: "{userdocs}\VGR Presenter\Shows"; Flags: recursesubdirs createallsubdirs skipifsourcedoesntexist onlyifdoesntexist uninsneveruninstall ignoreversion

[Icons]
Name: "{autoprograms}\VGRPresenter"; Filename: "{app}\appVGRPresenterUI.exe"
Name: "{autodesktop}\VGRPresenter"; Filename: "{app}\appVGRPresenterUI.exe"; Tasks: desktopicon

[Registry]
; .vgr show files open with the app (per-user, HKCU — no admin needed).
Root: HKCU; Subkey: "Software\Classes\.vgr"; ValueType: string; ValueName: ""; ValueData: "VGRPresenter.Show"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\VGRPresenter.Show"; ValueType: string; ValueName: ""; ValueData: "VGRPresenter Show"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\VGRPresenter.Show\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\appVGRPresenterUI.exe,0"
Root: HKCU; Subkey: "Software\Classes\VGRPresenter.Show\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\appVGRPresenterUI.exe"" ""%1"""

[Run]
Filename: "{app}\appVGRPresenterUI.exe"; Description: "{cm:LaunchProgram,VGRPresenter}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Nothing: the install dir is removed by the uninstaller; user data lives in
; %AppData%\VGR and %LocalAppData%\bps and is intentionally left alone.

[Code]
// ---- START FRESH --------------------------------------------------------------
// What "fresh" removes: the app's data folders — %AppData%\VGR\VGRPresenter and %AppData%\bps
// (settings and the kernel store) and everything under %LocalAppData%\bps (libraries, caches,
// the media index) EXCEPT its "recording" folder, which holds the operator's own takes and is
// never deleted. The seed data in [Files] then repopulates what a new install should have,
// because those entries only copy files that do not exist. Runs before any file is extracted.

const
  FreshKeepFolder = 'recording';

procedure DeleteAllExcept(const Dir, KeepName: String);
var
  FindRec: TFindRec;
  Path: String;
begin
  if not DirExists(Dir) then
    Exit;
  if FindFirst(Dir + '\*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') and
           (CompareText(FindRec.Name, KeepName) <> 0) then
        begin
          Path := Dir + '\' + FindRec.Name;
          if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
            DelTree(Path, True, True, True)
          else
            DeleteFile(Path);
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

// A command-line switch present? (/FRESH on the Setup command line = a clean reinstall without the page.)
function CmdLineSwitchPresent(const Name: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), Name) = 0 then
    begin
      Result := True;
      Exit;
    end;
end;

function WantFreshStart: Boolean;
begin
  Result := WizardIsTaskSelected('freshstart') or CmdLineSwitchPresent('/FRESH');
end;

// EVERYTHING the app stores, in every place it stores it. Recordings (the operator's own takes) are kept.
//   %AppData%\VGR\VGRPresenter       engine data (kernel store, Bibles, crash dumps, logs)
//   %AppData%\bps                    older builds' copy of the same
//   %LocalAppData%\bps               settings.json, libraries, templates, the media index, caches  (except "recording")
//   %LocalAppData%\VGR\VGRPresenter  Qt's own per-app caches (QML / shader / thumbnails) — a stale one from an
//                                    earlier build is exactly the kind of leftover that makes one laptop differ from another
//   {app}                            the previous install's files, so nothing an older build shipped can linger
// The boot-time autostart entry (HKCU ...\Run) is left to the app's own Settings switch.
procedure WipeAppState;
begin
  DelTree(ExpandConstant('{userappdata}\VGR\VGRPresenter'), True, True, True);
  DelTree(ExpandConstant('{userappdata}\bps'), True, True, True);
  DeleteAllExcept(ExpandConstant('{localappdata}\bps'), FreshKeepFolder);
  DelTree(ExpandConstant('{localappdata}\VGR\VGRPresenter'), True, True, True);
  if DirExists(ExpandConstant('{app}')) then
    DeleteAllExcept(ExpandConstant('{app}'), 'unins000.exe');   // the uninstaller registration stays
end;

// Ask once, on leaving the Tasks page, before anything is deleted. A silent install
// (/SILENT /TASKS="freshstart" or /FRESH) is an explicit choice already and is not asked again.
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectTasks) and WizardIsTaskSelected('freshstart') and (not WizardSilent) then
    Result := MsgBox('Clean reinstall will ERASE everything VGRPresenter stored on this computer — the old app ' +
                     'files, your settings, libraries and caches — and then install from scratch, so the app ' +
                     'starts exactly like a brand-new one.' + #13#10 + #13#10 +
                     'Your recordings are kept. This cannot be undone.' + #13#10 + #13#10 +
                     'Continue?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssInstall) and WantFreshStart then
    WipeAppState;
end;

// ---- UNINSTALL: offer the same erase ---------------------------------------------------------
// An uninstall keeps the user's data by default (so a reinstall finds everything as it was). When the
// person wants the app GONE — or wants to reinstall clean — one question removes the data too.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if (CurUninstallStep = usUninstall) and (not UninstallSilent) then
    if MsgBox('Also erase VGRPresenter''s settings, libraries and caches from this computer?' + #13#10 + #13#10 +
              'Choose Yes for a completely clean removal (or before a clean reinstall). Choose No to keep ' +
              'them for next time. Your recordings are kept either way.',
              mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
    begin
      DelTree(ExpandConstant('{userappdata}\VGR\VGRPresenter'), True, True, True);
      DelTree(ExpandConstant('{userappdata}\bps'), True, True, True);
      DeleteAllExcept(ExpandConstant('{localappdata}\bps'), FreshKeepFolder);
      DelTree(ExpandConstant('{localappdata}\VGR\VGRPresenter'), True, True, True);
    end;
end;
