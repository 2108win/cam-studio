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
; Windows 11: MFCreateVirtualCamera needs build 22000.
MinVersion=10.0.22000
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
Source: "{#SourceDir}\*"; DestDir: "{app}\App"; Excludes: "CamStudioVCam.dll"; Flags: ignoreversion recursesubdirs createallsubdirs
; The virtual camera lives next to the app folder and is registered for Frame Server
; (regserver = regsvr32 on install, unregister on uninstall).
Source: "{#SourceDir}\CamStudioVCam.dll"; DestDir: "{app}"; Flags: ignoreversion regserver 64bit

[Icons]
Name: "{autoprograms}\Cam Studio"; Filename: "{app}\App\CamStudio.exe"; Comment: "{cm:ShortcutComment}"
Name: "{autodesktop}\Cam Studio"; Filename: "{app}\App\CamStudio.exe"; Comment: "{cm:ShortcutComment}"; Tasks: desktopicon

[Registry]
; Same value the app writes from its own "Khởi động cùng Windows" switch.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "CamStudio"; ValueData: """{app}\App\CamStudio.exe"" --tray"; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\App\CamStudio.exe"; Description: "{cm:LaunchNow}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallDelete]
Type: filesandordirs; Name: "{app}\App"

[Code]
// Frame Server keeps the virtual camera DLL loaded and the app keeps its files open:
// close both before files are replaced or removed.
procedure StopCameraUsers();
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM CamStudio.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
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
    // regserver already unregistered the class; make sure nothing points at the deleted DLL.
    RegDeleteKeyIncludingSubkeys(HKLM, 'SOFTWARE\Classes\CLSID\{#VCamClsid}');
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'CamStudio');
  end;
end;
