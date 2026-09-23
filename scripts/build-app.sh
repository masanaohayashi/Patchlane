#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
swift build -c release --arch arm64 -j "${PATCHLANE_BUILD_JOBS:-1}"
APP="$PWD/build/Patchlane.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
BIN="$(swift build -c release --arch arm64 --show-bin-path)"
cp "$BIN/Patchlane" "$APP/Contents/MacOS/Patchlane"
swift scripts/make-icon.swift build/AppIcon.iconset
iconutil -c icns build/AppIcon.iconset -o "$APP/Contents/Resources/AppIcon.icns"
cp -R Sources/Patchlane/Resources/en.lproj Sources/Patchlane/Resources/ja.lproj "$APP/Contents/Resources/"
cp Resources/Info.plist "$APP/Contents/Info.plist"
rm -f "$APP/Contents/Resources/Patchlane.sdef"
scripts/sign-product.sh "$APP" Resources/Audio.entitlements
printf '%s\n' "$APP"
