; ═══════════════════════════════════════════════════════════════════════════
;  QuickSTT — Windows installer (Inno Setup 6)
;
;  Build it with the sibling Build-Installer.ps1 (it reads the version from
;  Cargo.toml and passes it in), or directly:
;
;      ISCC.exe /DMyAppVersion=2.0.0-alpha.18 quickstt.iss
;
;  Layout mirrors what the app expects NEXT TO THE EXE at runtime:
;      <install>\quickstt-slint.exe
;      <install>\wakeword_models\*.onnx|pt|json     (bundled wake-word heads)
;      <install>\tools\...                          (engine helper binaries)
;  Speech MODELS are NOT bundled — the app downloads them into the user's
;  profile on demand (Settings → Speech Models), which keeps the installer
;  small and lets users skip models they will never use.
; ═══════════════════════════════════════════════════════════════════════════

#define MyAppName "QuickSTT"
#ifndef MyAppVersion
  #define MyAppVersion "2.0.0-alpha.18"
#endif
; The Windows file-version resource needs four numeric fields; the app's own
; version carries a pre-release label ("2.0.0-alpha.18"). Build-Installer.ps1
; derives the numeric form and passes it in (/DMyAppVersionNumeric=2.0.0.18).
#ifndef MyAppVersionNumeric
  #define MyAppVersionNumeric "2.0.0.18"
#endif
#define MyAppNameVersion MyAppName + " " + MyAppVersion
#define MyAppPublisher "QuickSTT"
#define MyAppURL "https://github.com/quickstt/quickstt"
#define MyAppExeName "quickstt-slint.exe"

; Runtime data the app creates in the user profile (models, history, logs).
; The uninstaller offers to take these with it.
#define UserDataDir "QuickSTT"
#define UserDataDirLower "quickstt"

[Setup]
; NEVER ship an installer without a unique AppId — upgrades collide otherwise.
AppId={{8A1F0C4E-2B7D-4E3A-9F61-5C7D2E8B4A10}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
VersionInfoVersion={#MyAppVersionNumeric}
AppVerName={#MyAppNameVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
; Per-user install by default (no UAC): the app needs no system privileges —
; models, history and settings all live in the user profile. The dialog lets
; an admin install for every user instead.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
; The build output is written by Build-Installer.ps1; keep it out of the
; source tree so a rebuild never nests installers.
OutputDir=..\installer-output
OutputBaseFilename=QuickSTT-Setup-{#MyAppVersion}
SetupIconFile=..\..\icon_app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppNameVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; x64 only: the engines (llama.cpp / ONNX runtime) are x64 Windows builds.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
AppMutex=QuickSTT_SingleInstance_Mutex
RestartApplications=no
DisableProgramGroupPage=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "autostart"; Description: "Start QuickSTT automatically when I sign in"; GroupDescription: "Startup:"; Flags: unchecked

[Files]
; The app itself (release, stripped, LTO'd).
Source: "..\target\release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
; Bundled wake-word heads: the background listener loads these from beside the
; exe. NOTE: `discover_oww_heads` looks for the FLAT `<phrase>_v0.1.onnx`
; files, so the REPO ROOT copy is the one that ships (the quickstt-rust copy
; only has per-phrase subfolders, which the loader never reads). Without them
; wake words silently stay off.
Source: "..\..\wakeword_models\*"; DestDir: "{app}\wakeword_models"; Flags: ignoreversion recursesubdirs createallsubdirs
; Engine helper binaries the app resolves from <exe>\tools at runtime
; (Nemotron/Parakeet/photon/canary workers).
Source: "..\..\tools\*"; DestDir: "{app}\tools"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
; The blue mic icon, also used for every shortcut the installer creates.
Source: "..\..\icon_app.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\icon_app.ico"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\icon_app.ico"; Tasks: desktopicon

[Registry]
; Mirrors the dashboard's own "Start with Windows" toggle — both write the
; same Run key, so the dashboard keeps working after install.
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "QuickSTT"; ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
; Offer to start it right away (the first run shows the onboarding tour).
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Logs and installer scratch data only — real user data (models/history) is
; opt-in below, because re-downloading models is expensive.
Type: filesandordirs; Name: "{app}\*.log"
Type: filesandordirs; Name: "{app}\logs"

[Code]
var
  RemoveDataPage: TInputOptionWizardPage;

procedure InitializeWizard;
begin
  { The opt-in "also delete my models and history" question on uninstall. }
  RemoveDataPage := CreateInputOptionPage(wpSelectTasks,
    'Remove user data',
    'Delete downloaded speech models, transcripts and recordings?',
    'QuickSTT stores speech models, transcription history and audio in your user profile. Keep them if you plan to reinstall.',
    True, False);
  RemoveDataPage.Add('Delete downloaded models');
  RemoveDataPage.Add('Delete transcription history and audio');
  RemoveDataPage.Add('Delete settings (start with Windows, language picks, widgets)');
  RemoveDataPage.Values[0] := False;
  RemoveDataPage.Values[1] := False;
  RemoveDataPage.Values[2] := False;
end;

function UserDataPath(const Sub: String): String;
begin
  { ExpandConstant keeps the per-user path correct for the installing user. }
  Result := ExpandConstant('{userappdata}\' + Sub);
end;

procedure DeleteTreeIfExists(const Path: String);
begin
  if DirExists(Path) then
    DelTree(Path, True, True, True);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep <> usPostUninstall then
    exit;
  if RemoveDataPage = nil then
    exit;
  if RemoveDataPage.Values[0] then
    DeleteTreeIfExists(UserDataPath('{#UserDataDir}\models'));
  if RemoveDataPage.Values[1] then
  begin
    DeleteTreeIfExists(UserDataPath('{#UserDataDir}\history'));
    DeleteTreeIfExists(UserDataPath('{#UserDataDir}\recordings'));
    DeleteTreeIfExists(UserDataPath('{#UserDataDirLower}\history'));
    DeleteTreeIfExists(UserDataPath('{#UserDataDirLower}\recordings'));
  end;
  if RemoveDataPage.Values[2] then
  begin
    DeleteTreeIfExists(UserDataPath('{#UserDataDir}\config'));
    DeleteTreeIfExists(UserDataPath('{#UserDataDirLower}\config'));
  end;
end;
