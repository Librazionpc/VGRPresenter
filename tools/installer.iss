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

[Files]
; The whole deployed app (exe + Qt + MinGW runtime + plugins). Transient state
; the smoke test may have written into dist/ is excluded, and the build script
; also cleans it before compiling.
Source: "..\dist\VGRPresenter\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion; Excludes: "logs\*,enginedata\*,crashes\*"

; ---- Seed data (optional; the operator fills installer\seed-data\) ----------
; ONLY copies what the user does not already have; never removed on uninstall.
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
