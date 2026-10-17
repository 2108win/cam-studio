# Installs the virtual camera DLL for Frame Server. Run as Administrator:
#   powershell -ExecutionPolicy Bypass -File scripts\register.ps1
#
# Why admin: Frame Server runs as LocalService and only sees COM classes
# registered under HKLM, and it needs to read the DLL from a location every
# service account can access (Program Files).
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an elevated (Administrator) PowerShell."
}

$root = Split-Path -Parent $PSScriptRoot
# Log next to the build so the result can be read without the admin window.
Start-Transcript -Path (Join-Path $root "build\register.log") -Force | Out-Null
$built = Join-Path $root "build\$Config\CamStudioVCam.dll"
if (-not (Test-Path $built)) { throw "Build first: $built not found" }

$installDir = Join-Path $env:ProgramFiles "CamStudio"
New-Item -ItemType Directory -Force $installDir | Out-Null

# Frame Server keeps the loaded DLL open. Stop it so the old copy is released.
foreach ($svc in "FrameServerMonitor", "FrameServer") {
    $s = Get-Service $svc -ErrorAction SilentlyContinue
    if ($s -and $s.Status -eq "Running") { Stop-Service $svc -Force }
}

# A unique file name per install, so a still-locked old copy never blocks us.
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$target = Join-Path $installDir "CamStudioVCam-$stamp.dll"
Copy-Item $built $target

# regsvr32 is a GUI program: wait for it explicitly to get a real exit code.
$p = Start-Process "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList "/s", "`"$target`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "regsvr32 failed with exit code $($p.ExitCode)" }

# Remove older copies that are no longer in use.
Get-ChildItem $installDir -Filter "CamStudioVCam-*.dll" | Where-Object FullName -ne $target | ForEach-Object {
    try { Remove-Item $_.FullName -Force } catch { Write-Host "Still in use, left in place: $($_.Name)" }
}

$reg = Get-ItemProperty "HKLM:\SOFTWARE\Classes\CLSID\{72F5B2FD-C48E-449B-95CE-E8F13BF200CB}\InprocServer32"
Write-Host "Registered: $($reg.'(default)') (ThreadingModel=$($reg.ThreadingModel))"
Stop-Transcript | Out-Null
