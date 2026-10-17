# Installs what building the Android app needs: JDK 17 (winget) and the
# Android SDK (cmdline-tools, platform-tools, build-tools, platform 35) into
# %LOCALAPPDATA%\Android\Sdk. Running it means accepting the Android SDK
# licence: https://developer.android.com/studio/terms
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

# 1. JDK 17
if (-not (Get-Command java -ErrorAction SilentlyContinue) -and -not (Test-Path "C:\Program Files\Microsoft\jdk-17*")) {
    winget install --id Microsoft.OpenJDK.17 --exact --source winget --silent --accept-package-agreements --accept-source-agreements
}
$jdk = Get-ChildItem "C:\Program Files\Microsoft" -Directory -Filter "jdk-17*" | Select-Object -First 1
if (-not $jdk) { throw "JDK 17 not found after install" }
$env:JAVA_HOME = $jdk.FullName
[Environment]::SetEnvironmentVariable("JAVA_HOME", $jdk.FullName, "User")
Write-Host "JAVA_HOME = $($jdk.FullName)"

# 2. Android command-line tools (latest listed in Google's repository manifest)
$sdk = Join-Path $env:LOCALAPPDATA "Android\Sdk"
[Environment]::SetEnvironmentVariable("ANDROID_HOME", $sdk, "User")
$sdkmanager = Join-Path $sdk "cmdline-tools\latest\bin\sdkmanager.bat"
if (-not (Test-Path $sdkmanager)) {
    [xml]$repo = (Invoke-WebRequest "https://dl.google.com/android/repository/repository2-3.xml" -UseBasicParsing).Content
    $pkg = $repo.SelectNodes("//remotePackage[starts-with(@path,'cmdline-tools;')]") |
        Where-Object { $_.channelRef.ref -eq "channel-0" } |
        Sort-Object { [version](($_.revision.major, $_.revision.minor | Where-Object { $_ }) -join '.') } |
        Select-Object -Last 1
    $file = ($pkg.archives.archive | Where-Object { $_.'host-os' -eq "windows" }).complete.url
    $zip = Join-Path $env:TEMP $file
    Write-Host "Downloading $file"
    Invoke-WebRequest "https://dl.google.com/android/repository/$file" -OutFile $zip -UseBasicParsing
    $tmp = Join-Path $env:TEMP "cmdline-tools-extract"
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive $zip $tmp
    New-Item -ItemType Directory -Force (Join-Path $sdk "cmdline-tools") | Out-Null
    Move-Item (Join-Path $tmp "cmdline-tools") (Join-Path $sdk "cmdline-tools\latest")
    Remove-Item $zip, $tmp -Recurse -Force
}

# 3. SDK packages (licences accepted by the user's approval)
$yes = "y`n" * 20
$yes | & $sdkmanager --sdk_root=$sdk --licenses | Out-Null
& $sdkmanager --sdk_root=$sdk "platform-tools" "build-tools;35.0.0" "platforms;android-35"
Write-Host "Android SDK ready at $sdk"
& (Join-Path $sdk "platform-tools\adb.exe") version
