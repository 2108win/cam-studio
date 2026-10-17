#!/usr/bin/env bash
# Installs what xtool needs inside WSL (Ubuntu 24.04): usbmuxd + libimobiledevice,
# the Swift toolchain (via swiftly) and the xtool AppImage. Run as the normal
# WSL user (needs sudo). Afterwards run `xtool setup` yourself: it asks for your
# Apple ID and the path to Xcode.xip (e.g. /mnt/d/Downloads/Xcode_27.xip).
set -euo pipefail

sudo apt-get update
sudo apt-get install -y curl ca-certificates gnupg git unzip \
    usbmuxd libimobiledevice-utils libfuse2t64
# Libraries the Swift toolchain needs (list printed by `swiftly init` on Ubuntu 24.04).
sudo apt-get install -y zip gnupg2 libc6-dev libcurl4-openssl-dev libgcc-13-dev libpython3-dev \
    libstdc++-13-dev libxml2-dev libncurses-dev libz3-dev pkg-config zlib1g-dev

# Swift toolchain (latest release) via swiftly, the official installer from swift.org.
if ! command -v swift >/dev/null 2>&1; then
    cd "$(mktemp -d)"
    curl -fsSLO "https://download.swift.org/swiftly/linux/swiftly-$(uname -m).tar.gz"
    tar zxf "swiftly-$(uname -m).tar.gz"
    ./swiftly init --assume-yes --quiet-shell-followup
    # shellcheck disable=SC1091
    . "${SWIFTLY_HOME_DIR:-$HOME/.local/share/swiftly}/env.sh"
    hash -r
fi
swift --version

# xtool
curl -fL "https://github.com/xtool-org/xtool/releases/latest/download/xtool-$(uname -m).AppImage" -o /tmp/xtool
chmod +x /tmp/xtool
sudo mv /tmp/xtool /usr/local/bin/xtool
xtool --help | head -n 3
