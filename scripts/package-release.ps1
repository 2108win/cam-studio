# Builds everything in Release and assembles the files for a GitHub release in build\dist:
#   CamStudio-Setup-<version>.exe     (one-click installer: app + virtual camera, Inno Setup)
#   CamStudio-<version>-android.apk
# Needs Inno Setup 6 (winget install JRSoftware.InnoSetup).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\package-release.ps1
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$root = Split-Path -Parent $PSScriptRoot

$version = ([xml](Get-Content (Join-Path $root "app\CamStudio.App.csproj"))).Project.PropertyGroup.Version | Where-Object { $_ } | Select-Object -First 1
if (-not $version) { throw "No <Version> in app\CamStudio.App.csproj" }
Write-Host "Packaging Cam Studio $version"

# 1. Native parts (engine, virtual camera, tools).
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build.ps1")
if ($LASTEXITCODE) { throw "build.ps1 failed" }

# 2. App: the self-contained build output (.NET + Windows App SDK included). Not `dotnet publish`:
#    for this unpackaged WinUI app it leaves out CamStudio.pri and the compiled XAML (*.xbf), and the
#    app then crashes at startup.
$out = Join-Path $root "build\dist"   # not build\release: paths ignore case and build\Release holds the CMake output
$stage = Join-Path $out "CamStudio-$version-win-x64"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
$appOut = Join-Path $stage "CamStudio"
$bin = Join-Path $root "app\bin\x64\Release\net10.0-windows10.0.22621.0\win-x64"
Copy-Item $bin $appOut -Recurse
Get-ChildItem $appOut -Recurse -File -Filter *.pdb | Remove-Item -Force
Copy-Item (Join-Path $root "build\Release\CamStudioVCam.dll") $appOut
foreach ($f in "CamStudio.exe", "CamStudio.pri", "MainWindow.xbf", "CamStudioEngine.dll") {
    if (-not (Test-Path (Join-Path $appOut $f))) { throw "$f missing from the app folder" }
}

# 3. Installer (packaging\windows\CamStudio.iss).
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 not found: winget install JRSoftware.InnoSetup" }
& $iscc /Q "/DAppVersion=$version" "/DSourceDir=$appOut" "/DOutputDir=$out" (Join-Path $root "packaging\windows\CamStudio.iss")
if ($LASTEXITCODE) { throw "ISCC failed" }
Remove-Item $stage -Recurse -Force

# 4. Android.
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build-android.ps1")
if ($LASTEXITCODE) { throw "build-android.ps1 failed" }
Copy-Item (Join-Path $root "build\CamStudioPhone.apk") (Join-Path $out "CamStudio-$version-android.apk")

Get-ChildItem $out -File | ForEach-Object { "{0,-40} {1,8:N1} MB" -f $_.Name, ($_.Length / 1MB) }

