# Sourced by ios-run.sh and ios-archive.sh, from ios/ after xcodegen.

# A Release build setting of the app as Xcode resolves it (Signing.xcconfig + Signing.local.xcconfig).
ios_setting() {
    xcodebuild -showBuildSettings -project CamStudio.xcodeproj -target CamStudio -configuration Release 2>/dev/null |
        awk -v k="$1" '$1 == k && $2 == "=" { print $3; exit }'
}

# "<1 if a free Personal Team, else 0> <team name>" from the accounts signed in to Xcode; empty if
# Xcode does not know the team.
ios_team_info() {
    defaults export com.apple.dt.Xcode - 2>/dev/null | python3 -c '
import plistlib, sys
for teams in plistlib.loads(sys.stdin.buffer.read()).get("IDEProvisioningTeamByIdentifier", {}).values():
    for t in teams:
        if t.get("teamID") == sys.argv[1]:
            print(1 if t.get("isFreeProvisioningTeam") else 0, t.get("teamName", "")); sys.exit()
' "$1" || true
}

# With -allowProvisioningUpdates, Xcode registers a new bundle id on the signing team by itself,
# and an id registered on one account can never move to another (that is how vn.camstudio.phone
# was lost). So the first build of an id on a team asks first, unless a profile for it exists. The
# answer is remembered in Signing.local.xcconfig (dev builds may use the team's wildcard profile).
ios_confirm_team() {
    local team="$1" bundle="$2" mark="// confirmed: $1 $2" f
    grep -qxF "$mark" Signing.local.xcconfig 2>/dev/null && return 0
    for f in "$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles"/*; do
        [[ -f "$f" ]] && security cms -D -i "$f" 2>/dev/null | grep -qF "<string>$team.$bundle</string>" && return 0
    done
    local info; info="$(ios_team_info "$team")"
    echo "Xcode sắp đăng ký bundle id $bundle và $bundle.screen trên team $team (${info#? })."
    echo "Đã đăng ký thì không chuyển sang tài khoản khác được. Sai team thì sửa ios/Signing.local.xcconfig."
    [[ -t 0 ]] || { echo "Chạy lại trong terminal để xác nhận."; exit 1; }
    local answer; read -r -p "Đăng ký trên team này? [y/N] " answer
    [[ "$answer" == [yY]* ]] || exit 1
    echo "$mark" >> Signing.local.xcconfig
}
