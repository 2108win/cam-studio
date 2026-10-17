# Removes the virtual camera registration and installed DLLs. Run as Administrator.
$ErrorActionPreference = "Stop"

$key = "HKLM:\SOFTWARE\Classes\CLSID\{72F5B2FD-C48E-449B-95CE-E8F13BF200CB}"
if (Test-Path $key) { Remove-Item $key -Recurse -Force }

foreach ($svc in "FrameServerMonitor", "FrameServer") {
    $s = Get-Service $svc -ErrorAction SilentlyContinue
    if ($s -and $s.Status -eq "Running") { Stop-Service $svc -Force }
}

$installDir = Join-Path $env:ProgramFiles "CamStudio"
if (Test-Path $installDir) { Remove-Item $installDir -Recurse -Force }
Write-Host "Cam Studio virtual camera unregistered."
