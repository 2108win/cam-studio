#!/usr/bin/env bash
# Builds the iPhone app on a Mac (Xcode 26+, XcodeGen) and runs it on the iPhone paired with this
# Mac, over cable or Wi-Fi. Runs CryptoTests on the Mac first. The first run asks for the Apple
# team ID once and keeps it in ios/Signing.local.xcconfig (not in git).
#   scripts/ios-run.sh              tests, build, install, launch
#   scripts/ios-run.sh --no-test    skip CryptoTests
#   IOS_DEVICE=<udid> scripts/ios-run.sh   pick the iPhone when several are paired
set -euo pipefail
cd "$(dirname "$0")/../ios"

# Use the full Xcode even when xcode-select points at the command-line tools.
if [[ -z "${DEVELOPER_DIR:-}" && "$(xcode-select -p)" == *CommandLineTools* ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
command -v xcodegen >/dev/null || { echo "Cần XcodeGen: brew install xcodegen"; exit 1; }

if [[ ! -f Signing.local.xcconfig ]]; then
    echo "Nhập Team ID của Apple ID (Xcode > Settings > Accounts > Team), ví dụ ABCDE12345:"
    read -r team
    echo "DEVELOPMENT_TEAM = $team" > Signing.local.xcconfig
fi

xcodegen --quiet
dd=../build/ios-dd

if [[ "${1:-}" != "--no-test" ]]; then
    echo "== CryptoTests (Mac)"
    xcodebuild test -quiet -project CamStudio.xcodeproj -scheme CryptoTestsMac -destination platform=macOS -derivedDataPath "$dd"
fi

udid="${IOS_DEVICE:-$(xcrun devicectl list devices --json-output - 2>/dev/null | python3 -c '
import json, sys
for d in json.load(sys.stdin)["result"]["devices"]:
    h, c = d["hardwareProperties"], d["connectionProperties"]
    if h.get("platform") == "iOS" and h.get("reality") == "physical" and c.get("pairingState") == "paired":
        print(h["udid"])
        break
')}"
[[ -n "$udid" ]] || { echo "Không thấy iPhone đã ghép với Mac: cắm cáp, mở khóa iPhone, bấm Tin cậy."; exit 1; }

echo "== Build cho iPhone $udid"
xcodebuild build -quiet -project CamStudio.xcodeproj -scheme CamStudio -destination "platform=iOS,id=$udid" \
    -derivedDataPath "$dd" -allowProvisioningUpdates -allowProvisioningDeviceRegistration
echo "== Cài và mở app"
xcrun devicectl device install app --device "$udid" "$dd/Build/Products/Debug-iphoneos/CamStudio.app" >/dev/null
xcrun devicectl device process launch --terminate-existing --device "$udid" vn.camstudio.phone
