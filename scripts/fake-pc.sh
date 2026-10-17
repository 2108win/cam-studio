#!/usr/bin/env bash
# Builds (when changed) and runs tools/fake_pc.swift, a stand-in for the Windows PC app, to test the
# phone apps from a Mac. Arguments go to the tool, e.g.
#   scripts/fake-pc.sh --lens all --seconds 4
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -z "${DEVELOPER_DIR:-}" && "$(xcode-select -p)" == *CommandLineTools* && -d /Applications/Xcode.app ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
mkdir -p build
if [[ ! build/fake-pc -nt tools/fake_pc.swift || ! build/fake-pc -nt ios/CamStudio/Crypto.swift ]]; then
    swiftc -O -parse-as-library -o build/fake-pc tools/fake_pc.swift ios/CamStudio/Crypto.swift
fi
exec build/fake-pc "$@"
