; Inno Setup 6.7+ script for tmux for Windows
; Supports both user-level (no UAC) and machine-level (UAC) installs.
; The install type is selected on the first page of the wizard.
;
; Build from repo root:
;   iscc installer\tmux-windows.iss /DAppVersion=3.6a-win32-1.0.0

#ifndef AppVersion
  #define AppVersion GetFileVersion(AddBackslash(SourcePath) + "..\build\Release\tmux.exe")
#endif

#define AppName      "tmux for Windows"
#define AppPublisher "bitcode"
#define AppURL       "https://github.com/bitcode/tmux-windows"
#define AppExe       "tmux.exe"
#define AppProxy     "tmux-signal-proxy.exe"

[Setup]
AppId={{7A3F2B1C-4D5E-4F6A-8B9C-0D1E2F3A4B5C}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases

; --- Install mode ---
; PrivilegesRequired=lowest  means user-level by default.
; PrivilegesRequiredOverridesAllowed adds a radio-button page so the user can
; choose "Install for me only" (no UAC) or "Install for all users" (UAC prompt).
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

; {autopf} resolves to:
;   user mode  → %LOCALAPPDATA%\Programs
;   admin mode → %ProgramFiles%
DefaultDirName={autopf}\tmux
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes

; Output
OutputDir=..\dist
OutputBaseFilename=tmux-windows-{#AppVersion}-setup
Compression=lzma2/ultra64
SolidCompression=yes

; Upgrade — kill running tmux before overwriting
CloseApplications=yes
CloseApplicationsFilter=tmux.exe
RestartApplications=no

; Appearance
WizardStyle=modern
WizardSizePercent=110
SetupIconFile=tmux.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName} {#AppVersion}

; Minimum Windows version: 10 build 17763 (1809) for ConPTY + AF_UNIX
MinVersion=10.0.17763

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "addtopath"; Description: "Add tmux to PATH (recommended)"; \
    GroupDescription: "Shell integration:"; Flags: checkedonce

[Files]
Source: "..\build\Release\{#AppExe}";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\{#AppProxy}"; DestDir: "{app}"; Flags: ignoreversion
; Ship an example config only if the user has none yet
Source: "..\example_tmux.conf"; DestDir: "{app}"; DestName: "tmux.conf.example"; \
    Flags: ignoreversion onlyifdoesntexist

[Icons]
; No Start Menu shortcut — tmux is a CLI tool. Add one only if user wants it.
; Uncomment below to add a Windows Terminal launch shortcut:
; Name: "{group}\tmux"; Filename: "{app}\{#AppExe}"

[Registry]
; User PATH — added when installing for current user only
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; \
    ValueData: "{olddata};{app}"; \
    Check: NeedsUserPath; \
    Tasks: addtopath; Flags: preservestringtype uninsdeletevalue

; Machine PATH — added when installing for all users (admin mode)
Root: HKLM; \
    Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; \
    ValueType: expandsz; ValueName: "Path"; \
    ValueData: "{olddata};{app}"; \
    Check: NeedsMachinePath; \
    Tasks: addtopath; Flags: preservestringtype uninsdeletevalue

[Code]
// ---------------------------------------------------------------------------
// PATH check helpers — called from [Registry] Check: parameter
// ---------------------------------------------------------------------------
function PathContains(const Root: Integer; const SubKey, Dir: string): Boolean;
var
  OldPath: string;
begin
  if not RegQueryStringValue(Root, SubKey, 'Path', OldPath) then
    OldPath := '';
  Result := Pos(';' + Uppercase(Dir) + ';',
                ';' + Uppercase(OldPath) + ';') > 0;
end;

function NeedsUserPath: Boolean;
begin
  Result := not IsAdminInstallMode and
            not PathContains(HKCU, 'Environment', ExpandConstant('{app}'));
end;

function NeedsMachinePath: Boolean;
begin
  Result := IsAdminInstallMode and
            not PathContains(HKLM,
                'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
                ExpandConstant('{app}'));
end;

// ---------------------------------------------------------------------------
// Broadcast WM_SETTINGCHANGE so open shells pick up the new PATH immediately
// without a reboot or re-login.
// ---------------------------------------------------------------------------
procedure RefreshEnv;
var
  S: string;
begin
  S := 'Environment';
  // PostBroadcastMessage is async — returns immediately without waiting for
  // every top-level window to respond, so it cannot hang the installer.
  PostBroadcastMessage($001A {WM_SETTINGCHANGE}, 0, CastStringToInteger(S));
end;

// ---------------------------------------------------------------------------
// Remove a specific directory from a PATH registry value.
// Splits on ';', filters out the target (case-insensitive), rejoins.
// ---------------------------------------------------------------------------
procedure RemoveFromPath(const Root: Integer; const SubKey, Dir: string);
var
  OldPath, NewPath, Remaining, Entry: string;
  Sep: Integer;
  DirUp: string;
begin
  if not RegQueryStringValue(Root, SubKey, 'Path', OldPath) then
    Exit;
  DirUp     := Uppercase(Dir);
  Remaining := OldPath + ';';  // sentinel — every entry ends with ;
  NewPath   := '';
  repeat
    Sep := Pos(';', Remaining);
    if Sep = 0 then Break;
    Entry     := Copy(Remaining, 1, Sep - 1);
    Remaining := Copy(Remaining, Sep + 1, Length(Remaining));
    if (Entry <> '') and (Uppercase(Entry) <> DirUp) then
    begin
      if NewPath <> '' then NewPath := NewPath + ';';
      NewPath := NewPath + Entry;
    end;
  until Remaining = '';
  RegWriteStringValue(Root, SubKey, 'Path', NewPath);
end;

// ---------------------------------------------------------------------------
// Uninstall: strip {app} from PATH and broadcast the change
// ---------------------------------------------------------------------------
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  AppDir: string;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    AppDir := ExpandConstant('{app}');
    RemoveFromPath(HKCU, 'Environment', AppDir);
    RemoveFromPath(HKLM,
        'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
        AppDir);
    RefreshEnv;
  end;
end;

// ---------------------------------------------------------------------------
// After install: broadcast PATH change so the current shell picks it up
// ---------------------------------------------------------------------------
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    RefreshEnv;
end;

// ---------------------------------------------------------------------------
// Finish page: show install scope in the summary memo
// ---------------------------------------------------------------------------
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo,
    MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo,
    MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine + NewLine + MemoTasksInfo;
  if IsAdminInstallMode then
    Result := Result + NewLine + NewLine +
        'Install scope: all users (administrator privileges required).'
  else
    Result := Result + NewLine + NewLine +
        'Install scope: current user only (no administrator privileges required).';
end;
