#!/bin/bash
# Signed installer + uninstaller ZIP, notarized with credentials already in Keychain.
set -euo pipefail
cd "$(dirname "$0")/.."
export COPYFILE_DISABLE=1
if [[ -f .local/release.env ]]; then source .local/release.env; fi
: "${PATCHLANE_SIGN_IDENTITY:?Set your Developer ID Application identity}"
: "${PATCHLANE_INSTALLER_IDENTITY:?Set your Developer ID Installer identity}"
: "${PATCHLANE_NOTARY_PROFILE:?Set your notarytool Keychain profile}"
export PATCHLANE_SIGN_IDENTITY PATCHLANE_INSTALLER_IDENTITY
profile="$PATCHLANE_NOTARY_PROFILE"
version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' Resources/Info.plist)
release="$PWD/build/release/Patchlane-$version"
logs="$PWD/build/release/notary-$version"
mkdir -p "$release" "$logs"
nice -n 15 bash scripts/package-driver.sh
cp build/Patchlane-Installer.pkg "$release/Patchlane-Installer.pkg"
ditto 'build/Patchlane Uninstaller.app' "$release/Patchlane Uninstaller.app"
for product in build/Patchlane.app build/Patchlane.driver build/Patchlane-2ch.driver build/ring-broker "$release/Patchlane Uninstaller.app"; do
    codesign --verify --strict "$product"
done
pkgutil --check-signature "$release/Patchlane-Installer.pkg"
archive="$PWD/build/release/Patchlane-$version.zip"
ditto -c -k --keepParent "$release" "$archive"
xcrun notarytool submit "$archive" --keychain-profile "$profile" --output-format json > "$logs/submission.json"
submission=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$logs/submission.json")
# Keep the submission ID even if polling is interrupted.
xcrun notarytool wait "$submission" --keychain-profile "$profile" --output-format json > "$logs/result.json"
xcrun notarytool log "$submission" --keychain-profile "$profile" "$logs/log.json"
python3 - "$logs/result.json" <<'PY'
import json,sys
result=json.load(open(sys.argv[1]))
if result.get('status')!='Accepted': raise SystemExit('Notarization not accepted; inspect '+sys.argv[1])
PY
for product in "$release/Patchlane-Installer.pkg" "$release/Patchlane Uninstaller.app"; do
    xcrun stapler staple "$product"
    xcrun stapler validate "$product"
done
spctl --assess --type install --verbose=2 "$release/Patchlane-Installer.pkg"
spctl --assess --type execute --verbose=2 "$release/Patchlane Uninstaller.app"
# ZIPs cannot hold a stapled ticket themselves; rebuild with the stapled products.
rm -f "$archive"
ditto -c -k --keepParent "$release" "$archive"
shasum -a 256 "$archive" > "$archive.sha256"
printf '\nRelease: %s\n' "$archive"
