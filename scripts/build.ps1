# Builds into build\Release. Usage: .\scripts\build.ps1 [-Config Debug] [-Target camstudio-verify]
param([string]$Config = "Release", [string]$Target = "")
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -products * -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "MSVC not found. Install Visual Studio Build Tools with the C++ workload." }

Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null

# -Target app builds only the WinUI app (it uses the engine DLL already built).
if ($Target -ne "app") {
    cmake -S $root -B "$root\build" -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    $targetArgs = @()
    if ($Target) { $targetArgs = @("--target", $Target) }
    cmake --build "$root\build" --config $Config --parallel @targetArgs
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    # The Windows 10 DirectShow filter again as x86 (build-x86): a 32-bit app loads a 32-bit
    # filter. The consumer tool too, to test it.
    if (-not $Target -or $Target -eq "CamStudioDShow") {
        cmake -S $root -B "$root\build-x86" -G "Visual Studio 17 2022" -A Win32
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
        cmake --build "$root\build-x86" --config $Config --parallel --target CamStudioDShow camstudio-dshow-consumer
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
    }
}

# WinUI 3 app (copies build\<Config>\CamStudioEngine.dll next to CamStudio.exe).
if (-not $Target -or $Target -eq "app") {
    dotnet build "$root\app\CamStudio.App.csproj" -c $Config -p:Platform=x64
    exit $LASTEXITCODE
}
