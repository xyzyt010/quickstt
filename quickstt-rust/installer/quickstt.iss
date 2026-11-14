; ═══════════════════════════════════════════════════════════════════════════
;  QuickSTT — Windows installer (Inno Setup 6)
;
;  Build it with the sibling Build-Installer.ps1 (it reads the version from
;  Cargo.toml and passes it in), or directly:
;
;      ISCC.exe /DMyAppVersion=2.0.0-alpha.19 quickstt.iss
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
  #define MyAppVersion "2.0.0-alpha.26"
#endif
; The Windows file-version resource needs four numeric fields; the app's own
; version carries a pre-release label ("2.0.0-alpha.26"). Build-Installer.ps1
; derives the numeric form and passes it in (/DMyAppVersionNumeric=2.0.0.26).
#ifndef MyAppVersionNumeric
  #define MyAppVersionNumeric "2.0.0.26"
#endif
#define MyAppNameVersion MyAppName + " " + MyAppVersion
#define MyAppPublisher "QuickSTT"
#define MyAppURL "https://github.com/xyzyt010/quickstt"
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
VersionInfoProductVersion={#MyAppVersionNumeric}
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
; Real wizard pages (Inno's `modern` style hides some by default).
DisableWelcomePage=no
DisableDirPage=no
DisableProgramGroupPage=yes
DisableReadyPage=no
DisableFinishedPage=no
LicenseFile=..\..\LICENSE
InfoBeforeFile=preinstall.txt
; What this release actually is, on the welcome page.
InfoAfterFile=readme.txt

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Full installation"
Name: "compact"; Description: "Compact installation (CPU engines only)"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "core"; Description: "QuickSTT (required)"; Types: full compact custom; Flags: fixed
Name: "engines"; Description: "Speech engine runtimes (Parakeet, Nemotron, whisper, Cactus, LFM3-Audio)"; Types: full compact custom
Name: "gpuacc"; Description: "GPU acceleration libraries (adds ~90 MB; CPU-only works without them)"; Types: full; Flags: dontinheritcheck
Name: "wakeword"; Description: "Wake-word heads (hey jarvis, alexa)"; Types: full custom

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart"; Description: "Start QuickSTT automatically when I sign in"; GroupDescription: "Startup:"; Flags: unchecked

[Files]
; ── core: the app itself (release, stripped, LTO'd) ──
Source: "..\target\release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion; Components: core
; The blue mic icon, also used for every shortcut the installer creates.
Source: "..\..\icon_app.ico"; DestDir: "{app}"; Flags: ignoreversion; Components: core
Source: "..\assets\MaterialSymbolsOutlined.ttf"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist; Components: core
Source: "..\assets\Inter-VariableFont.ttf"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist; Components: core
Source: "..\assets\Inter-VariableFont.ttf.license"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist; Components: core
Source: "..\..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion skipifsourcedoesntexist; Components: core

; ── engine runtimes the app resolves from <exe>\tools at runtime ──
Source: "..\..\tools\photon\*"; DestDir: "{app}\tools\photon"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: engines
Source: "..\..\tools\nemotron\*"; Excludes: "ggml-vulkan.dll,*.a,*.bak,*.def,*.py"; DestDir: "{app}\tools\nemotron"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: engines
Source: "..\..\tools\parakeet\*"; Excludes: "DirectML.dll"; DestDir: "{app}\tools\parakeet"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: engines
Source: "..\..\tools\whistle\*"; DestDir: "{app}\tools\whistle"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: engines
Source: "..\..\tools\canary\*"; DestDir: "{app}\tools\canary"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: engines
; LFM3-Audio: llama.cpp b11960 (v0.8.0) CPU binary + its ggml dispatch DLLs.
; Both llama-cli.exe and llama-mtmd*.dll / ggml-cpu-*.dll kernels ship together —
; llama-cli loads the encoder and kernels at startup from this folder.
Source: "..\..\tools\lfm3_audio\*"; DestDir: "{app}\tools\lfm3_audio"; Excludes: "*.a,*.bak,*.def,*.py"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: engines

; ── GPU backends (optional): Vulkan (llama.cpp) + DirectML (ONNX Runtime) ──
; Same destination paths as the core tools, so selecting the component puts
; them back exactly where the engine probes for them. Skipping it leaves a
; fully working CPU-only install that is ~10 MB smaller.
Source: "..\..\tools\nemotron\ggml-vulkan.dll"; DestDir: "{app}\tools\nemotron"; Flags: ignoreversion skipifsourcedoesntexist; Components: gpuacc
Source: "..\..\tools\parakeet\DirectML.dll"; DestDir: "{app}\tools\parakeet"; Flags: ignoreversion skipifsourcedoesntexist; Components: gpuacc

; ── wake-word heads: the listener loads these from beside the exe ──
; NOTE: `discover_oww_heads` looks for the FLAT `<phrase>_v0.1.onnx` files,
; so the REPO ROOT copy is the one that ships (the quickstt-rust copy only has
; per-phrase subfolders, which the loader never reads). Without them wake
; words silently stay off.
Source: "..\..\wakeword_models\*"; DestDir: "{app}\wakeword_models"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Components: wakeword

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\icon_app.ico"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\icon_app.ico"; Tasks: desktopicon

[Registry]
; Mirrors the dashboard's own "Start with Windows" toggle — both write the
; same Run key, so the dashboard keeps working after install.
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "QuickSTT"; ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
; Launch on Finish. `nowait` returns immediately; `postinstall` shows the
; checkbox on the Finished page; `skipifsilent` keeps /SILENT quiet.
; NOTE: if a previous QuickSTT is still running, the new instance hands off to
; it (single-instance IPC) and the onboarding tour is NOT re-shown — quit the
; old instance first, or use "Show the intro tour" in the tray/dashboard.
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}}"; Flags: nowait postinstall skipifsilent shellexec

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
