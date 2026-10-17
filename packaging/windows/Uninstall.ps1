# Removes Cam Studio: virtual camera registration, app files, shortcut and Apps & features entry.
# Asks for Administrator by itself when needed.
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [Text.Encoding]::UTF8   # Vietnamese messages in the console window

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    return
}

Get-Process CamStudio -ErrorAction SilentlyContinue | Stop-Process -Force

$clsid = "HKLM:\SOFTWARE\Classes\CLSID\{72F5B2FD-C48E-449B-95CE-E8F13BF200CB}"
if (Test-Path $clsid) { Remove-Item $clsid -Recurse -Force }

foreach ($svc in "FrameServerMonitor", "FrameServer") {
    $s = Get-Service $svc -ErrorAction SilentlyContinue
    if ($s -and $s.Status -eq "Running") { Stop-Service $svc -Force }
}

# Start with Windows (HKCU Run value written by the app's setting).
Remove-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "CamStudio" -ErrorAction SilentlyContinue

Remove-Item (Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Cam Studio.lnk") -Force -ErrorAction SilentlyContinue
Remove-Item "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\CamStudio" -Recurse -Force -ErrorAction SilentlyContinue

$installDir = Join-Path $env:ProgramFiles "CamStudio"
# This script lives in $installDir: delete the folder from a separate process after we exit.
Start-Process cmd.exe -WindowStyle Hidden -ArgumentList "/c timeout /t 2 /nobreak >nul & rmdir /s /q `"$installDir`""
Write-Host "Đã gỡ Cam Studio. Cài đặt riêng của bạn (HKCU\Software\CamStudio, %LOCALAPPDATA%\CamStudio) được giữ lại."
