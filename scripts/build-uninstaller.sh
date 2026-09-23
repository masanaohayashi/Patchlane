#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
app="$PWD/build/Patchlane Uninstaller.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
xcrun swiftc -O -target arm64-apple-macosx13.0 Uninstaller/main.swift \
  -framework AppKit -o "$app/Contents/MacOS/PatchlaneUninstaller"
cp Uninstaller/uninstall.sh "$app/Contents/Resources/uninstall.sh"
python3 - "$app" <<'PY'
import plistlib,sys
from pathlib import Path
plist={"CFBundleIdentifier":"audio.patchlane.uninstaller",
       "CFBundleExecutable":"PatchlaneUninstaller",
       "CFBundleName":"Patchlane Uninstaller",
       "CFBundleDisplayName":"Patchlane アンインストーラー",
       "CFBundlePackageType":"APPL", "CFBundleVersion":"1",
       "CFBundleShortVersionString":"0.3.1", "LSMinimumSystemVersion":"13.0",
       "NSHighResolutionCapable":True}
(Path(sys.argv[1])/"Contents/Info.plist").write_bytes(plistlib.dumps(plist))
PY
scripts/sign-product.sh "$app"
codesign --verify --strict "$app"
"$app/Contents/MacOS/PatchlaneUninstaller" --self-test
printf '%s\n' "$app"
