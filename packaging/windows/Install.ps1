# Cam Studio installer (release zip). Started by Install.cmd, which asks for Administrator.
#
# - Copies the app to %ProgramFiles%\CamStudio\App and adds Start menu shortcut + Apps & features entry.
# - Registers the virtual camera DLL for Frame Server (needs HKLM + a folder every service can read).
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [Text.Encoding]::UTF8   # Vietnamese messages in the console window

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Hãy chạy Install.cmd (nó tự xin quyền Administrator)."
}

$source = Join-Path $PSScriptRoot "CamStudio"
if (-not (Test-Path (Join-Path $source "CamStudio.exe"))) { throw "Không thấy thư mục CamStudio cạnh Install.ps1. Hãy giải nén cả file zip trước." }
$version = (Get-Content (Join-Path $PSScriptRoot "VERSION.txt") -ErrorAction SilentlyContinue | Select-Object -First 1)
if (-not $version) { $version = "0.0.0" }

$installDir = Join-Path $env:ProgramFiles "CamStudio"
$appDir = Join-Path $installDir "App"
Write-Host "Cài Cam Studio $version vào $installDir"

# The app and Frame Server hold files open: close the app and stop the camera services.
Get-Process CamStudio -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
foreach ($svc in "FrameServerMonitor", "FrameServer") {
    $s = Get-Service $svc -ErrorAction SilentlyContinue
    if ($s -and $s.Status -eq "Running") { Stop-Service $svc -Force }
}

# App files (a clean copy each time).
New-Item -ItemType Directory -Force $installDir | Out-Null
if (Test-Path $appDir) { Remove-Item $appDir -Recurse -Force }
Copy-Item $source $appDir -Recurse
Get-ChildItem $appDir -Recurse -File | Unblock-File   # drop the "downloaded from the Internet" mark

# Virtual camera: a unique file name per install, so a still-locked old copy never blocks us.
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$vcam = Join-Path $installDir "CamStudioVCam-$stamp.dll"
Copy-Item (Join-Path $appDir "CamStudioVCam.dll") $vcam
Unblock-File $vcam
$p = Start-Process "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList "/s", "`"$vcam`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "Đăng ký camera ảo thất bại (regsvr32 mã $($p.ExitCode))." }
Get-ChildItem $installDir -Filter "CamStudioVCam-*.dll" | Where-Object FullName -ne $vcam | ForEach-Object {
    try { Remove-Item $_.FullName -Force } catch { }
}

# Uninstaller next to the app.
Copy-Item (Join-Path $PSScriptRoot "Uninstall.ps1") $installDir -Force
Unblock-File (Join-Path $installDir "Uninstall.ps1")

# Start menu shortcut (all users).
$exe = Join-Path $appDir "CamStudio.exe"
$lnk = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Cam Studio.lnk"
$shell = New-Object -ComObject WScript.Shell
$s = $shell.CreateShortcut($lnk)
$s.TargetPath = $exe
$s.WorkingDirectory = $appDir
$s.IconLocation = "$exe,0"
$s.Description = "Cam Studio: camera ảo ở độ phân giải gốc"
$s.Save()

# Apps & features entry.
$key = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\CamStudio"
New-Item $key -Force | Out-Null
$size = [int]((Get-ChildItem $installDir -Recurse -File | Measure-Object Length -Sum).Sum / 1KB)
$props = @{
    DisplayName     = "Cam Studio"
    DisplayVersion  = $version
    Publisher       = "Cam Studio"
    DisplayIcon     = "$exe,0"
    InstallLocation = $installDir
    UninstallString = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$installDir\Uninstall.ps1`""
}
foreach ($k in $props.Keys) { Set-ItemProperty $key $k $props[$k] }
Set-ItemProperty $key EstimatedSize $size -Type DWord
Set-ItemProperty $key NoModify 1 -Type DWord
Set-ItemProperty $key NoRepair 1 -Type DWord

Write-Host ""
Write-Host "Đã cài xong Cam Studio $version." -ForegroundColor Green
Write-Host "Mở app từ Start menu (Cam Studio). Gỡ cài đặt trong Settings > Apps > Installed apps."
