# Builds the phone app (android/) into build\CamStudioPhone.apk.
# First run downloads Gradle and creates the Gradle wrapper.
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$root = Split-Path -Parent $PSScriptRoot
$android = Join-Path $root "android"
$gradleVersion = "9.8.1"

if (-not $env:JAVA_HOME) {
    $jdk = Get-ChildItem "C:\Program Files\Microsoft" -Directory -Filter "jdk-17*" | Select-Object -First 1
    if ($jdk) { $env:JAVA_HOME = $jdk.FullName }
}
if (-not $env:ANDROID_HOME) { $env:ANDROID_HOME = Join-Path $env:LOCALAPPDATA "Android\Sdk" }
"sdk.dir=$($env:ANDROID_HOME -replace '\\','\\')" | Set-Content (Join-Path $android "local.properties") -Encoding ascii

if (-not (Test-Path (Join-Path $android "gradlew.bat"))) {
    $dist = Join-Path $root "build\gradle-$gradleVersion"
    if (-not (Test-Path $dist)) {
        $zip = Join-Path $root "build\gradle-$gradleVersion-bin.zip"
        Invoke-WebRequest "https://services.gradle.org/distributions/gradle-$gradleVersion-bin.zip" -OutFile $zip -UseBasicParsing
        Expand-Archive $zip (Join-Path $root "build")
        Remove-Item $zip
    }
    Push-Location $android
    & (Join-Path $dist "bin\gradle.bat") wrapper --gradle-version $gradleVersion --no-daemon -q
    Pop-Location
}

Push-Location $android
try {
    & .\gradlew.bat assembleRelease --no-daemon -q
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}
$apk = Join-Path $android "app\build\outputs\apk\release\app-release.apk"
Copy-Item $apk (Join-Path $root "build\CamStudioPhone.apk") -Force
Write-Host "Built build\CamStudioPhone.apk ($([math]::Round((Get-Item $apk).Length / 1KB)) KB)"
