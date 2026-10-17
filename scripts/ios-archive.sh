#!/usr/bin/env bash
# Archives the iPhone app (with the screen-sharing extension) for TestFlight / App Store Connect.
# The archive shows up in Xcode > Window > Organizer: upload it from there (Distribute App).
# Needs a paid Apple Developer Program team and a bundle id registered on it, both set in
# ios/Signing.local.xcconfig (not in git; see ios/Signing.xcconfig):
#   DEVELOPMENT_TEAM = ABCDE12345
#   CAMSTUDIO_BUNDLE_ID = com.example.camstudio
#
#   scripts/ios-archive.sh               archive; build number = date.time (each upload needs a new one)
#   scripts/ios-archive.sh --build 7     pick the build number
#   scripts/ios-archive.sh --unsigned    Release build check without an Apple account (not uploadable)
set -euo pipefail
cd "$(dirname "$0")/../ios"

# Use the full Xcode even when xcode-select points at the command-line tools.
if [[ -z "${DEVELOPER_DIR:-}" && "$(xcode-select -p)" == *CommandLineTools* ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
command -v xcodegen >/dev/null || { echo "Cần XcodeGen: brew install xcodegen"; exit 1; }

build="$(date +%Y%m%d.%H%M)"
unsigned=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build) build="$2"; shift 2 ;;
        --unsigned) unsigned=1; shift ;;
        *) echo "Tham số không hợp lệ: $1"; exit 1 ;;
    esac
done

xcodegen --quiet
source ../scripts/ios-team.sh
pb=/usr/libexec/PlistBuddy

if (( unsigned )); then
    archive=../build/ios-archive/CamStudio.xcarchive
    rm -rf "$archive"
    sign=(CODE_SIGNING_ALLOWED=NO)
else
    team="$(ios_setting DEVELOPMENT_TEAM)"
    bundle="$(ios_setting PRODUCT_BUNDLE_IDENTIFIER)"
    [[ -n "$team" ]] || { echo "Chưa có DEVELOPMENT_TEAM trong ios/Signing.local.xcconfig."; exit 1; }
    if [[ "$bundle" == vn.camstudio.phone ]]; then
        echo "Bundle id vn.camstudio.phone thuộc tài khoản khác, không đăng ký được cho TestFlight."
        echo "Thêm vào ios/Signing.local.xcconfig một id của bạn, ví dụ:  CAMSTUDIO_BUNDLE_ID = com.tenban.camstudio"
        exit 1
    fi
    # A Personal Team (free account) can build for a device but cannot upload to App Store Connect.
    if [[ "$(ios_team_info "$team")" == 1* ]]; then
        echo "Team $team là Personal Team (tài khoản miễn phí): không upload lên TestFlight được."
        echo "Cần tài khoản Apple Developer Program trả phí."
        exit 1
    fi
    ios_confirm_team "$team" "$bundle"
    echo "== Team $team, bundle id $bundle (tiện ích: $bundle.screen), build $build"
    archive="$HOME/Library/Developer/Xcode/Archives/$(date +%Y-%m-%d)/CamStudio $build.xcarchive"
    sign=(-allowProvisioningUpdates)
fi

echo "== Archive (Release)"
xcodebuild archive -quiet -project CamStudio.xcodeproj -scheme CamStudio -configuration Release \
    -destination generic/platform=iOS -archivePath "$archive" -derivedDataPath ../build/ios-dd \
    CURRENT_PROJECT_VERSION="$build" "${sign[@]}"

# What App Store Connect checks first: the extension is embedded with the app's version, and the
# permission strings survived (Xcode can strip them from Info.plist, see ios/README.md).
app="$archive/Products/Applications/CamStudio.app"
appex="$app/PlugIns/CamStudioScreen.appex"
[[ -d "$appex" ]] || { echo "Thiếu tiện ích chia sẻ màn hình trong $app/PlugIns"; exit 1; }
for key in CFBundleShortVersionString CFBundleVersion; do
    [[ "$($pb -c "Print $key" "$app/Info.plist")" == "$($pb -c "Print $key" "$appex/Info.plist")" ]] ||
        { echo "$key của app và tiện ích khác nhau"; exit 1; }
done
for key in NSCameraUsageDescription NSLocalNetworkUsageDescription NSBonjourServices; do
    $pb -c "Print $key" "$app/Info.plist" >/dev/null 2>&1 ||
        { echo "Info.plist của app thiếu $key: chạy  git checkout ios/Info.plist  rồi archive lại."; exit 1; }
done
echo "== Xong: $($pb -c 'Print CFBundleIdentifier' "$app/Info.plist")" \
    "$($pb -c 'Print CFBundleShortVersionString' "$app/Info.plist") ($build)"
echo "   $archive"

if (( ! unsigned )); then
    echo "Upload: Xcode > Window > Organizer > chọn bản này > Distribute App > TestFlight & App Store."
    open "$archive"
fi
