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
