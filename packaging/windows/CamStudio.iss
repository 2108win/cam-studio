; Cam Studio installer (Inno Setup 6). Built by scripts\package-release.ps1:
;   ISCC /DAppVersion=0.1.3 /DSourceDir=<staged app folder> /DOutputDir=build\dist CamStudio.iss
; One .exe: installs the app to Program Files, registers the virtual camera for
; Frame Server, adds Start menu / desktop shortcuts and an Apps & features entry.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #error SourceDir (the staged CamStudio app folder) is required
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

#define VCamClsid "{72F5B2FD-C48E-449B-95CE-E8F13BF200CB}"
#define DShowClsid "{7D2B9C41-3A6E-4F58-B0D1-9E4C2F6A8B13}"
#define DShowCategory "{860BB310-5D01-11d0-BD3B-00A0C911CE86}"

[Setup]
AppId={{6E3C8C2A-3F0B-4B8E-9C61-5A2D7F1E9B44}
AppName=Cam Studio
AppVersion={#AppVersion}
AppVerName=Cam Studio {#AppVersion}
AppPublisher=Cam Studio
AppPublisherURL=https://github.com/2108win/cam-studio
AppSupportURL=https://github.com/2108win/cam-studio/issues
AppUpdatesURL=https://github.com/2108win/cam-studio/releases
DefaultDirName={autopf}\CamStudio
DisableProgramGroupPage=yes
DisableDirPage=auto
; The virtual camera is registered machine-wide (Frame Server only reads HKLM).
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1809 (build 17763) or newer, 64-bit. Windows 11 gets the Media Foundation virtual
; camera (every app sees it); Windows 10 gets a DirectShow filter instead (docs/windows10.md).
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=CamStudio-Setup-{#AppVersion}
SetupIconFile=..\..\app\Assets\CamStudio.ico
UninstallDisplayIcon={app}\App\CamStudio.exe
UninstallDisplayName=Cam Studio
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
RestartIfNeededByRun=no

[Languages]
Name: "vi"; MessagesFile: "Vietnamese.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
vi.DesktopIcon=Tạo biểu tượng trên màn hình nền
en.DesktopIcon=Create a desktop shortcut
vi.StartWithWindows=Chạy cùng Windows (ẩn ở khay, chỉ bật camera khi có ứng dụng dùng)
en.StartWithWindows=Start with Windows (in the tray; the camera only wakes when an app uses it)
vi.LaunchNow=Mở Cam Studio ngay
en.LaunchNow=Open Cam Studio now
vi.ShortcutComment=Dùng webcam hoặc điện thoại làm camera ảo ở độ phân giải gốc
en.ShortcutComment=Use a webcam or a phone as a native-resolution virtual camera

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopIcon}"; Flags: unchecked
Name: "autostart"; Description: "{cm:StartWithWindows}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}\App"; Excludes: "CamStudioVCam.dll,CamStudioDShow64.dll,CamStudioDShow32.dll"; Flags: ignoreversion recursesubdirs createallsubdirs
; The virtual camera lives next to the app folder and is registered for Frame Server
; (regserver = regsvr32 on install, unregister on uninstall). Windows 11 only.
Source: "{#SourceDir}\CamStudioVCam.dll"; DestDir: "{app}"; Flags: ignoreversion regserver 64bit; Check: IsWindows11
; Windows 10: the DirectShow source filter, for 64-bit and for 32-bit apps.
Source: "{#SourceDir}\CamStudioDShow64.dll"; DestDir: "{app}"; Flags: ignoreversion regserver 64bit; Check: not IsWindows11
Source: "{#SourceDir}\CamStudioDShow32.dll"; DestDir: "{app}"; Flags: ignoreversion regserver 32bit; Check: not IsWindows11

[Icons]
Name: "{autoprograms}\Cam Studio"; Filename: "{app}\App\CamStudio.exe"; Comment: "{cm:ShortcutComment}"
Name: "{autodesktop}\Cam Studio"; Filename: "{app}\App\CamStudio.exe"; Comment: "{cm:ShortcutComment}"; Tasks: desktopicon

[Registry]
; Same value the app writes from its own "Khởi động cùng Windows" switch.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "CamStudio"; ValueData: """{app}\App\CamStudio.exe"" --tray"; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\App\CamStudio.exe"; Description: "{cm:LaunchNow}"; Flags: nowait postinstall skipifsilent runasoriginaluser
; An update started by the app (/SILENT /RELAUNCH=show|tray, app\Services\Updater.cs): open it again.
Filename: "{app}\App\CamStudio.exe"; Parameters: "{code:RelaunchArgs}"; Flags: nowait runasoriginaluser; Check: RelaunchAfterUpdate

[UninstallDelete]
Type: filesandordirs; Name: "{app}\App"

[Code]
function IsWindows11(): Boolean;
var
  V: TWindowsVersion;
begin
  GetWindowsVersionEx(V);
  Result := V.Build >= 22000;
end;

function RelaunchAfterUpdate(): Boolean;
begin
  Result := WizardSilent and (ExpandConstant('{param:RELAUNCH|}') <> '');
end;

function RelaunchArgs(Param: String): String;
begin
  // --updated: the app does not install another update by itself in that session.
  if CompareText(ExpandConstant('{param:RELAUNCH|}'), 'tray') = 0 then
    Result := '--tray --updated'
  else
    Result := '--updated';
end;

// Frame Server keeps the virtual camera DLL loaded and the app keeps its files open:
// close both before files are replaced or removed.
procedure StopCameraUsers();
var
  Code: Integer;
  Adb: String;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM CamStudio.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  // The adb server the app starts for Android phones on a cable runs from App\adb. Stop only
  // that one: "adb kill-server" would stop any server on the port (Android Studio's too).
  Adb := ExpandConstant('{app}\App\adb\adb.exe');
  if FileExists(Adb) then
    Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
      '-NoProfile -NonInteractive -Command "Get-Process adb -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq ''' + Adb + ''' } | Stop-Process -Force"',
      '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop FrameServerMonitor', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop FrameServer', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Sleep(1500);
end;

// Leftovers of the older zip + Install.cmd installs: a timestamped DLL copy and
// their own Apps & features entry.
procedure RemoveOldZipInstall();
var
  FindRec: TFindRec;
  Dir: String;
begin
  Dir := ExpandConstant('{autopf}\CamStudio');
  if FindFirst(Dir + '\CamStudioVCam-*.dll', FindRec) then
  begin
    try
      repeat
        DeleteFile(Dir + '\' + FindRec.Name);
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
  DeleteFile(Dir + '\Uninstall.ps1');
  RegDeleteKeyIncludingSubkeys(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\CamStudio');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopCameraUsers();
  RemoveOldZipInstall();
  Result := '';
end;

function InitializeUninstall(): Boolean;
begin
  StopCameraUsers();
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    // regserver already unregistered the classes; make sure nothing points at the deleted DLLs.
    RegDeleteKeyIncludingSubkeys(HKLM, 'SOFTWARE\Classes\CLSID\{#VCamClsid}');
    RegDeleteKeyIncludingSubkeys(HKLM, 'SOFTWARE\Classes\CLSID\{#DShowClsid}');
    RegDeleteKeyIncludingSubkeys(HKLM, 'SOFTWARE\Classes\CLSID\{#DShowCategory}\Instance\{#DShowClsid}');
    RegDeleteKeyIncludingSubkeys(HKLM, 'SOFTWARE\Classes\WOW6432Node\CLSID\{#DShowClsid}');
    RegDeleteKeyIncludingSubkeys(HKLM, 'SOFTWARE\Classes\WOW6432Node\CLSID\{#DShowCategory}\Instance\{#DShowClsid}');
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'CamStudio');
  end;
end;
